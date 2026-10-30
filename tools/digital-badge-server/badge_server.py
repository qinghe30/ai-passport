# -*- coding: utf-8 -*-
"""badge_server.py —— 数字工牌演示后台(仅用 Python 标准库,零第三方依赖)。

与固件的接口约定(见 ai-passport/main/badge_upload.c):
  POST /api/recordings/stream   工牌流式上传录音(chunked),请求头:
                                X-Device-Id / X-Sample-Rate / X-Sample-Bits /
                                X-Channels / X-Recording-Mode(stream|replay)
                                正常收到结束块才算完整,完整后触发"转写+分析+任务生成";
                                中途断开的录音视为不完整,直接丢弃不处理。
  GET  /api/badge/tasks?device= 任务摘要 JSON:{"tasks":[{id,title,assignee,due,status}]}
                                status != "done" 视为工牌侧待办;最多返回 8 条。

浏览器管理页(演示用):
  GET  /tasks       任务管理:确认/编辑标题/负责人/截止时间/状态,手动新建
  GET  /recordings  录音与沟通分析结果浏览

配置(config.json,与本文件同目录,缺省项用内置默认值):
  port             监听端口(固件 BADGE_SERVER_URL 需指向本机 IP + 此端口)
  feishu_webhook   飞书群机器人 Webhook 地址;为空则不推送
  asr_api_key      OpenAI 兼容 ASR 的密钥;为空则使用模拟转写
  llm_api_key      OpenAI 兼容 LLM 的密钥;为空则使用模拟分析
"""
import json
import os
import re
import sys
import time
import threading
import urllib.request
from datetime import date, timedelta
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(BASE_DIR, "data")
REC_DIR = os.path.join(DATA_DIR, "recordings")
ANA_DIR = os.path.join(DATA_DIR, "analysis")
TASKS_PATH = os.path.join(DATA_DIR, "tasks.json")   # 历史遗留,任务库已迁至飞书多维表格
CONFIG_PATH = os.path.join(BASE_DIR, "config.json")

# 与固件约定对齐的常量。
TASKS_API_MAX = 8          # 单次返回给工牌的任务条数上限(badge_tasks.h BADGE_TASK_MAX)
TASK_TITLE_MAX_BYTES = 96  # 标题 UTF-8 字节上限(badge_config.h BADGE_TASK_TITLE_MAX_BYTES)

FEISHU_LOCK = threading.Lock() # 串行化 Webhook 推送,避免打乱群内顺序

DEFAULT_CONFIG = {
    "port": 8000,
    "host": "0.0.0.0",
    "feishu_webhook": "",
    "feishu_app_id": "",        # 飞书自建应用 App ID(可发录音/文本文件)
    "feishu_app_secret": "",    # 飞书自建应用 App Secret
    "feishu_chat_id": "",       # 目标群 chat_id;为空自动取机器人所在的第一个群
    "feishu_base_app_token": "",   # 任务库多维表格 app_token(任务确认/指派在飞书完成)
    "feishu_base_table_id": "",    # 任务库数据表 table_id
    "asr_provider": "bailian",  # bailian=百炼 paraformer 文件转写;openai=OpenAI 兼容接口
    "asr_base_url": "https://api.openai.com/v1",
    "asr_api_key": "",          # 为空时回退读环境变量 DASHSCOPE_API_KEY
    "asr_model": "whisper-1",   # 仅 openai 模式使用
    "bailian_asr_model": "paraformer-v2",
    "llm_base_url": "https://api.openai.com/v1",  # 百炼模式下自动换成 DashScope 兼容端点
    "llm_api_key": "",          # 为空时回退读环境变量 DASHSCOPE_API_KEY
    "llm_model": "gpt-4o-mini", # 百炼模式下自动换成 glm-5.3
}
CONFIG = dict(DEFAULT_CONFIG)


# --------------------------------------------------------------------------
# 存储:沟通分析结果(任务库已迁至飞书多维表格,见下方"任务库"一节)
# --------------------------------------------------------------------------

def clip_title(text, limit=TASK_TITLE_MAX_BYTES):
    """把标题按 UTF-8 字节上限截断,不在多字节字符中间切断。"""
    raw = text.encode("utf-8")
    if len(raw) <= limit:
        return text
    return raw[:limit].decode("utf-8", errors="ignore")


def fmt_duration(seconds):
    """把秒数格式化成中文可读时长:不足一分钟显示秒,超过一分钟显示分秒,超过一小时显示时分秒。"""
    total = int(round(float(seconds or 0)))
    if total < 60:
        return "%d秒" % total
    if total < 3600:
        return "%d分%d秒" % (total // 60, total % 60)
    return "%d小时%d分%d秒" % (total // 3600, total % 3600 // 60, total % 60)


def latest_recording_id(device=""):
    """取该设备最近一次录音的 ID。

    录音文件名为 设备ID_YYYYMMDD_HHMMSS,字典序最大即时间最新。
    """
    if not os.path.isdir(ANA_DIR):
        return ""
    names = [n[:-5] for n in os.listdir(ANA_DIR) if n.endswith(".json")]
    if device:
        names = [n for n in names if n.startswith(device + "_")]
    return max(names) if names else ""


def save_analysis(record):
    path = os.path.join(ANA_DIR, record["recording_id"] + ".json")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(record, f, ensure_ascii=False, indent=2)
    os.replace(tmp, path)


def load_analysis(recording_id):
    path = os.path.join(ANA_DIR, recording_id + ".json")
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


# --------------------------------------------------------------------------
# 飞书推送:优先自建应用(可发文件),其次群机器人 Webhook(仅文本)
# --------------------------------------------------------------------------

FEISHU_TOKEN = {"value": "", "expire": 0.0}   # tenant_access_token 缓存
FEISHU_CHAT = {"id": ""}                      # 目标群 chat_id 缓存


def feishu_app_ready():
    return bool(CONFIG["feishu_app_id"] and CONFIG["feishu_app_secret"])


def feishu_token():
    """获取/缓存自建应用 tenant_access_token,到期前 60 秒刷新。"""
    if FEISHU_TOKEN["value"] and time.time() < FEISHU_TOKEN["expire"]:
        return FEISHU_TOKEN["value"]
    body = json.dumps({"app_id": CONFIG["feishu_app_id"],
                       "app_secret": CONFIG["feishu_app_secret"]}).encode("utf-8")
    req = urllib.request.Request(
        "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal",
        data=body, headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as resp:
        result = json.loads(resp.read().decode("utf-8"))
    if result.get("code") != 0:
        raise ValueError("飞书 token 获取失败: %s" % result.get("msg"))
    FEISHU_TOKEN["value"] = result["tenant_access_token"]
    FEISHU_TOKEN["expire"] = time.time() + int(result.get("expire", 7200)) - 60
    return FEISHU_TOKEN["value"]


def feishu_chat_id():
    """确定目标群:优先配置项,否则取机器人加入的第一个群。"""
    if CONFIG["feishu_chat_id"]:
        return CONFIG["feishu_chat_id"]
    if FEISHU_CHAT["id"]:
        return FEISHU_CHAT["id"]
    req = urllib.request.Request(
        "https://open.feishu.cn/open-apis/im/v1/chats?page_size=50",
        headers={"Authorization": "Bearer " + feishu_token()})
    with urllib.request.urlopen(req, timeout=10) as resp:
        result = json.loads(resp.read().decode("utf-8"))
    items = result.get("data", {}).get("items", [])
    if not items:
        raise ValueError("机器人不在任何群里,请先把机器人拉进群")
    FEISHU_CHAT["id"] = items[0]["chat_id"]
    print("[feishu] 自动选择群: %s" % (items[0].get("name") or FEISHU_CHAT["id"]))
    return FEISHU_CHAT["id"]


def feishu_send_msg(msg_type, content):
    """用自建应用向目标群发一条消息;失败抛异常由调用方记日志。"""
    body = json.dumps({
        "receive_id": feishu_chat_id(),
        "msg_type": msg_type,
        "content": json.dumps(content, ensure_ascii=False),
    }, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=chat_id",
        data=body, headers={
            "Content-Type": "application/json; charset=utf-8",
            "Authorization": "Bearer " + feishu_token(),
        })
    with FEISHU_LOCK:
        with urllib.request.urlopen(req, timeout=15) as resp:
            result = json.loads(resp.read().decode("utf-8"))
    if result.get("code") != 0:
        raise ValueError("飞书消息发送失败(%s): %s" % (msg_type, result.get("msg")))
    return result.get("data", {}).get("message_id")


def http_post_multipart(url, fields, file_field, file_path, headers=None, timeout=300):
    """手工拼 multipart/form-data 提交文件;fields 按序写在文件字段之前。"""
    boundary = "----BadgeBoundary%d" % int(time.time() * 1000)
    parts = []
    for name, value in fields:
        parts.append(("--%s\r\n" % boundary).encode())
        parts.append(('Content-Disposition: form-data; name="%s"\r\n\r\n' % name).encode())
        parts.append(str(value).encode())
        parts.append(b"\r\n")
    name = os.path.basename(file_path)
    parts.append(("--%s\r\n" % boundary).encode())
    parts.append(('Content-Disposition: form-data; name="%s"; filename="%s"\r\n'
                  % (file_field, name)).encode())
    parts.append(b"Content-Type: application/octet-stream\r\n\r\n")
    with open(file_path, "rb") as f:
        parts.append(f.read())
    parts.append(("\r\n--%s--\r\n" % boundary).encode())
    body = b"".join(parts)
    hdrs = {"Content-Type": "multipart/form-data; boundary=%s" % boundary}
    if headers:
        hdrs.update(headers)
    req = urllib.request.Request(url, data=body, headers=hdrs)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.status, resp.read()


# 飞书文件消息硬上限 30 MB;留出 multipart 头余量,单卷按 25 MB 切。
FEISHU_FILE_MAX = 25 * 1024 * 1024


def _wav_info(wav_path):
    """解析 WAV 头,返回 (fmt 块内容, data 块起始偏移, data 字节数)。"""
    with open(wav_path, "rb") as f:
        head = f.read(4096)
    if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
        raise ValueError("录音文件不是标准 WAV,无法分卷")
    fmt = None
    data_pos = 0
    data_size = 0
    pos = 12
    while pos + 8 <= len(head):
        cid = head[pos:pos + 4]
        clen = int.from_bytes(head[pos + 4:pos + 8], "little")
        if cid == b"fmt ":
            fmt = head[pos + 8:pos + 8 + clen]
        elif cid == b"data":
            data_pos = pos + 8
            data_size = clen
            break
        pos += 8 + clen + (clen & 1)
    if fmt is None or data_pos == 0:
        raise ValueError("WAV 头缺少 fmt 或 data 块")
    return fmt, data_pos, data_size


def _wav_header(fmt, data_len):
    """用原 fmt 块重建 44 字节标准 WAV 头,data_len 为本片 PCM 字节数。"""
    return (b"RIFF" + (36 + data_len).to_bytes(4, "little") + b"WAVE"
            + b"fmt " + len(fmt).to_bytes(4, "little") + fmt
            + b"data" + data_len.to_bytes(4, "little"))


def wav_slice(wav_path, max_bytes=FEISHU_FILE_MAX):
    """把 WAV 按 PCM 帧切成若干段,每段写一个临时 WAV 文件。

    返回 [(路径, 是否临时分卷)]。文件未超限时原样返回,不产生临时文件。
    """
    total = os.path.getsize(wav_path)
    if total <= max_bytes:
        return [(wav_path, False)]
    fmt, data_pos, data_size = _wav_info(wav_path)
    # 一帧 = 声道数 x 位深/8;切分必须按帧对齐,否则分片音频会出错。
    channels = int.from_bytes(fmt[2:4], "little")
    bits = int.from_bytes(fmt[14:16], "little")
    frame = max(1, channels * bits // 8)
    per_slice = (max_bytes - 44) // frame * frame
    remain = min(data_size, total - data_pos)
    parts = []
    offset = data_pos
    index = 0
    with open(wav_path, "rb") as src:
        while remain > 0:
            src.seek(offset)
            pcm = src.read(min(per_slice, remain))
            if not pcm:
                break
            index += 1
            tmp = "%s_part%d.wav" % (wav_path[:-4], index)
            with open(tmp, "wb") as dst:
                dst.write(_wav_header(fmt, len(pcm)))
                dst.write(pcm)
            parts.append((tmp, True))
            offset += len(pcm)
            remain -= len(pcm)
    return parts


def feishu_upload_file(file_path, file_type="stream"):
    """上传文件到飞书消息资源库,返回 file_key。"""
    status, raw = http_post_multipart(
        "https://open.feishu.cn/open-apis/im/v1/files",
        [("file_type", file_type), ("file_name", os.path.basename(file_path))],
        "file", file_path,
        headers={"Authorization": "Bearer " + feishu_token()})
    result = json.loads(raw.decode("utf-8"))
    if result.get("code") != 0:
        raise ValueError("飞书文件上传失败: %s" % result.get("msg"))
    return result["data"]["file_key"]


def feishu_send(text):
    """推送一条纯文本:有自建应用走应用消息,否则走群 Webhook,都没有只记日志。"""
    if feishu_app_ready():
        try:
            feishu_send_msg("text", {"text": text})
            print("[feishu] 应用消息推送完成")
        except (OSError, ValueError) as e:
            print("[feishu] 应用消息推送失败: %s" % e)
        return
    url = CONFIG["feishu_webhook"]
    if not url:
        print("[feishu] 未配置应用或 Webhook,跳过推送")
        return
    body = json.dumps({"msg_type": "text", "content": {"text": text}}).encode("utf-8")
    req = urllib.request.Request(url, data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with FEISHU_LOCK:
            with urllib.request.urlopen(req, timeout=10) as resp:
                print("[feishu] Webhook 推送完成,HTTP %d" % resp.status)
    except OSError as e:
        print("[feishu] Webhook 推送失败: %s" % e)


# --------------------------------------------------------------------------
# 任务库:飞书多维表格。任务写入、确认、指派都在飞书内完成,本地不再落 tasks.json。
# --------------------------------------------------------------------------

# 多维表格单选状态 -> 工牌侧状态码的映射。
BASE_STATUS_TO_BADGE = {"待确认": "pending", "已确认": "confirmed", "已完成": "done"}


def base_ready():
    return bool(CONFIG.get("feishu_base_app_token") and CONFIG.get("feishu_base_table_id"))


def _base_call(method, path, body=None):
    """调多维表格数据表接口;code 非 0 直接抛错由调用方记日志。"""
    url = "https://open.feishu.cn/open-apis/bitable/v1/apps/%s/tables/%s/%s" % (
        CONFIG["feishu_base_app_token"], CONFIG["feishu_base_table_id"], path)
    headers = {"Authorization": "Bearer " + feishu_token()}
    data = None
    if body is not None:
        headers["Content-Type"] = "application/json; charset=utf-8"
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=30) as resp:
        result = json.loads(resp.read().decode("utf-8"))
    if result.get("code") != 0:
        raise ValueError("多维表格接口报错 %s: %s" % (result.get("code"), result.get("msg")))
    return result.get("data") or {}


def _field_text(v):
    """文本字段读值:接口可能返回纯字符串或富文本分段数组,统一拼成字符串。"""
    if isinstance(v, list):
        return "".join(str(seg.get("text", "")) for seg in v if isinstance(seg, dict))
    return "" if v is None else str(v)


def base_list_records():
    """分页拉全部任务记录(任务量小,全量拉取后本地过滤即可)。"""
    items, page_token = [], ""
    while True:
        path = "records?page_size=500"
        if page_token:
            path += "&page_token=" + page_token
        data = _base_call("GET", path)
        items.extend(data.get("items") or [])
        if not data.get("has_more"):
            return items
        page_token = data.get("page_token") or ""


def _task_no_of(rec):
    try:
        return int(_field_text((rec.get("fields") or {}).get("任务号")) or 0)
    except ValueError:
        return 0


def base_create_task(title, assignee, due, recording_id):
    """待办写入多维表格,状态固定从「待确认」开始;返回 (任务号, record_id)。"""
    no = max([_task_no_of(r) for r in base_list_records()] or [0]) + 1
    fields = {
        "标题": clip_title(title),
        "任务号": str(no),
        "状态": "待确认",
        "责任方": assignee or "",
        "录音ID": recording_id,
    }
    if due:
        # 日期字段要求毫秒时间戳。
        fields["截止日期"] = int(time.mktime(time.strptime(due, "%Y-%m-%d")) * 1000)
    data = _base_call("POST", "records", {"fields": fields})
    return no, data["record"]["record_id"]


def base_claim_task(record_id, name):
    """认领任务:确认并把负责人记为点击人。"""
    _base_call("PUT", "records/" + record_id,
               {"fields": {"状态": "已确认", "负责人": name}})


def base_delete_task(record_id):
    """误提任务:直接从任务库删除该条记录。"""
    _base_call("DELETE", "records/" + record_id)


# 已推送的纪要卡片内容:{message_id: card_payload},用于认领/误提后更新卡片。
_CARD_STORE = {}


def tasks_for_device(device):
    """工牌展示:最近一次录音生成、且未完成的任务,按截止日升序,最多 TASKS_API_MAX 条。"""
    rid = latest_recording_id(device)
    if not rid or not base_ready():
        return []
    alive = []
    for rec in base_list_records():
        f = rec.get("fields") or {}
        if _field_text(f.get("录音ID")) != rid:
            continue
        status = _field_text(f.get("状态")) or "待确认"
        if status == "已完成":
            continue
        due_ms = f.get("截止日期") or 0
        due = time.strftime("%Y-%m-%d", time.localtime(due_ms / 1000)) if due_ms else ""
        alive.append({
            "id": _task_no_of(rec),
            "title": _field_text(f.get("标题")),
            "assignee": _field_text(f.get("负责人")) or _field_text(f.get("责任方")),
            "due": due,
            "status": BASE_STATUS_TO_BADGE.get(status, "pending"),
        })
    alive.sort(key=lambda t: (t["due"] or "9999-12-31", t["id"]))
    return alive[:TASKS_API_MAX]


def _send_summary_card(record):
    """构造并推送纪要交互卡片;卡片内容存入 _CARD_STORE 供认领/误提后更新。"""
    md = ["设备 %s | 时长 %s | %s" % (
        record["device"], fmt_duration(record["seconds"]), record["created_at"]),
          "**摘要:** %s" % record["summary"]]
    sections = record.get("sections") or []
    if sections:
        for s in sections:
            md.append("\n**【%s】**" % s.get("title", ""))
            for p in s.get("points", []):
                md.append("· %s" % p)
    elif record.get("key_points"):
        # 兼容旧记录:没有分板块时回退显示关键信息。
        md.append("\n**关键信息:**")
        for kp in record["key_points"]:
            md.append("· %s" % kp)
    elements = [{"tag": "div", "text": {"tag": "lark_md", "content": "\n".join(md)}}]

    task_records = record.get("task_records") or []
    if task_records:
        elements.append({"tag": "hr"})
        elements.append({"tag": "div", "text": {"tag": "lark_md",
            "content": "**待办事项**(点「认领」确认并指派给自己;提取有误点「误提」)"}})
        # 卡片内待办按本次录音从 1 重新编号;value 里保留全局任务号便于回查多维表格。
        for idx, tr in enumerate(task_records, 1):
            prefix = ("[%s] " % tr["assignee"]) if tr.get("assignee") else ""
            elements.append({
                "tag": "column_set", "flex_mode": "none", "background_style": "default",
                "columns": [
                    {"tag": "column", "width": "weighted", "weight": 1,
                     "vertical_align": "center",
                     "elements": [{"tag": "div", "text": {"tag": "lark_md",
                         "content": "**%d.** %s%s(截止 %s)" % (
                             idx, prefix, tr["title"], tr["due"])}}]},
                    {"tag": "column", "width": "auto", "vertical_align": "center",
                     "elements": [{"tag": "button",
                                   "text": {"tag": "plain_text", "content": "认领"},
                                   "type": "primary",
                                   "value": {"act": "claim", "idx": idx, "title": tr["title"],
                                             "rid": tr["record_id"], "no": tr["no"]}}]},
                    {"tag": "column", "width": "auto", "vertical_align": "center",
                     "elements": [{"tag": "button",
                                   "text": {"tag": "plain_text", "content": "误提"},
                                   "type": "danger",
                                   "value": {"act": "reject", "idx": idx, "title": tr["title"],
                                             "rid": tr["record_id"], "no": tr["no"]}}]},
                ]})
    else:
        elements.append({"tag": "div", "text": {"tag": "lark_md",
            "content": "本次沟通未提取到明确待办"}})

    # 销售评分卡片段(有 sales_score 才渲染)。
    score = record.get("sales_score")
    if score:
        lines = ["---", "**拜访能力评分 %d 分**" % score["overall"],
                 "综合分 = %s" % score["overall_formula"], ""]
        for key in _SALES_DIMENSION_WEIGHTS:
            d = score["dimensions"][key]
            name = _SALES_DIMENSION_NAMES[key]
            lines.append("**%s %d**(%d%%)" % (name, d["score"], int(d["weight"] * 100)))
            if d["notes"]:
                lines.append("%s" % d["notes"])
        if score.get("strength_items"):
            lines.append("\n**亮点:**")
            for it in score["strength_items"][:3]:
                lines.append("· %s" % it["item"])
        if score.get("missing_items"):
            lines.append("\n**待改进:**")
            for it in score["missing_items"][:3]:
                lines.append("· [%s] %s" % (
                    _SALES_DIMENSION_NAMES.get(it["category"], it["category"]), it["item"]))
        elements.append({"tag": "hr"})
        elements.append({"tag": "div", "text": {"tag": "lark_md", "content": "\n".join(lines)}})

    card_payload = {
        "config": {"wide_screen_mode": True},
        "header": {"title": {"tag": "plain_text",
                             "content": "数字工牌·沟通纪要 %s" % record["recording_id"]},
                   "template": "blue"},
        "elements": elements}
    mid = feishu_send_msg("interactive", card_payload)
    if mid:
        # 记下卡片内容,认领/误提回调时用来把对应按钮行替换成结果文字。
        _CARD_STORE[mid] = card_payload
        while len(_CARD_STORE) > 20:
            _CARD_STORE.pop(next(iter(_CARD_STORE)))


def feishu_push_recording(record, wav_path):
    """录音分析完成后的三件套推送:纪要交互卡片 + 录音文件 + 识别文字文件。"""
    if not feishu_app_ready():
        # 未配置自建应用:退回纯文本简报(Webhook 或仅记日志)。
        lines = ["数字工牌新沟通纪要",
                 "设备: %s" % record["device"],
                 "时长: %s" % fmt_duration(record["seconds"]),
                 "摘要: %s" % record["summary"]]
        for t in record["task_ids"]:
            lines.append("新增任务 #%s,请在后台确认并指派" % t)
        feishu_send("\n".join(lines))
        return
    try:
        # 1. 沟通纪要交互卡片(待办带认领/误提按钮)
        _send_summary_card(record)
        # 2. 录音文件本体:超过飞书单文件上限时按时间分卷发送
        slices = wav_slice(wav_path)
        try:
            if len(slices) > 1:
                feishu_send_msg("text", {"text": "录音较长,分 %d 段发送" % len(slices)})
            for i, (path, is_part) in enumerate(slices, 1):
                if is_part:
                    feishu_send_msg("text", {"text": "第 %d / %d 段" % (i, len(slices))})
                feishu_send_msg("file", {"file_key": feishu_upload_file(path)})
        except (OSError, ValueError) as e:
            # 录音体积大、上传易失败;失败不影响后面的识别文字送达。
            print("[feishu] 录音文件推送失败: %s" % e)
        finally:
            for path, is_part in slices:
                if is_part:
                    os.unlink(path)
        # 3. 识别文字全文(txt 文件,长会议也能完整发出)
        # 优先用纠错后的文本,并附上纠错对照,便于核对口音误识处。
        transcript = record.get("corrected_transcript") or record.get("transcript") or ""
        corrections = record.get("corrections") or []
        if transcript.strip():
            txt_path = wav_path[:-4] + ".txt"
            with open(txt_path, "w", encoding="utf-8") as f:
                f.write(transcript)
                if corrections:
                    f.write("\n\n=== 口音纠错对照 ===\n")
                    for c in corrections:
                        f.write("- %s -> %s (%s)\n" % (
                            c.get("original", ""), c.get("corrected", ""), c.get("reason", "")))
            try:
                feishu_send_msg("file", {"file_key": feishu_upload_file(txt_path)})
            finally:
                os.unlink(txt_path)
        print("[feishu] 三件套推送完成: %s" % record["recording_id"])
    except (OSError, ValueError) as e:
        print("[feishu] 三件套推送失败: %s" % e)


# --------------------------------------------------------------------------
# 转写与分析(百炼 paraformer / OpenAI 兼容接口;未配 key 时走模拟模式)
# --------------------------------------------------------------------------

# 单段转写上限:10 分钟 PCM 约 18.3 MB,低于 Whisper 25 MB 的请求体积上限。
ASR_CHUNK_BYTES = 10 * 60 * 32000

# 百炼 API 根地址:优先取环境变量 DASHSCOPE_BASE_URL 的专属域名。
DASHSCOPE_ROOT = os.environ.get(
    "DASHSCOPE_BASE_URL", "https://dashscope.aliyuncs.com/compatible-mode/v1")
DASHSCOPE_ROOT = DASHSCOPE_ROOT.split("/compatible-mode")[0].rstrip("/")


def bailian_upload_file(file_path):
    """把本地文件上传到百炼临时存储空间,返回 oss:// 临时 URL(48 小时有效)。"""
    model = CONFIG["bailian_asr_model"]
    req = urllib.request.Request(
        "%s/api/v1/uploads?action=getPolicy&model=%s" % (DASHSCOPE_ROOT, model),
        headers={"Authorization": "Bearer " + CONFIG["asr_api_key"]})
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = json.loads(resp.read().decode("utf-8"))["data"]
    key = "%s/%s" % (data["upload_dir"], os.path.basename(file_path))
    status, raw = http_post_multipart(
        data["upload_host"],
        [("OSSAccessKeyId", data["oss_access_key_id"]),
         ("Signature", data["signature"]),
         ("policy", data["policy"]),
         ("x-oss-object-acl", data["x_oss_object_acl"]),
         ("x-oss-forbid-overwrite", data["x_oss_forbid_overwrite"]),
         ("key", key),
         ("success_action_status", "200")],
        "file", file_path, timeout=600)
    if status != 200:
        raise ValueError("百炼文件上传失败: HTTP %d %s" % (status, raw[:200]))
    return "oss://" + key


def bailian_transcribe(wav_path):
    """百炼 paraformer 录音文件转写:整段提交(支持数小时),轮询取回全文。

    开启说话人分离,返回按"说话人N:"分行的文本,便于区分工牌佩戴者与客户。
    """
    file_url = bailian_upload_file(wav_path)
    payload = {
        "model": CONFIG["bailian_asr_model"],
        "input": {"file_urls": [file_url]},
        "parameters": {"channel_id": [0], "language_hints": ["zh", "en"],
                       "diarization_enabled": True},
    }
    req = urllib.request.Request(
        "%s/api/v1/services/audio/asr/transcription" % DASHSCOPE_ROOT,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Authorization": "Bearer " + CONFIG["asr_api_key"],
                 "Content-Type": "application/json",
                 "X-DashScope-Async": "enable",
                 # 使用 oss:// 临时 URL 必须显式要求解析
                 "X-DashScope-OssResourceResolve": "enable"})
    with urllib.request.urlopen(req, timeout=60) as resp:
        task_id = json.loads(resp.read().decode("utf-8"))["output"]["task_id"]
    print("[asr] 百炼转写任务已提交: %s" % task_id)

    # 轮询任务状态;2 小时录音按经验几分钟内完成,上限放宽到 1 小时。
    deadline = time.time() + 3600
    while True:
        if time.time() > deadline:
            raise ValueError("百炼转写超时(task_id=%s)" % task_id)
        time.sleep(5)
        req = urllib.request.Request(
            "%s/api/v1/tasks/%s" % (DASHSCOPE_ROOT, task_id),
            headers={"Authorization": "Bearer " + CONFIG["asr_api_key"]})
        with urllib.request.urlopen(req, timeout=30) as resp:
            output = json.loads(resp.read().decode("utf-8"))["output"]
        status = output["task_status"]
        if status == "SUCCEEDED":
            result = output["results"][0]
            if result.get("subtask_status") != "SUCCEEDED":
                raise ValueError("百炼转写子任务失败: %s %s" % (
                    result.get("code"), result.get("message")))
            break
        if status in ("FAILED", "CANCELED"):
            raise ValueError("百炼转写失败: %s %s" % (
                output.get("code"), output.get("message")))

    with urllib.request.urlopen(result["transcription_url"], timeout=60) as resp:
        doc = json.loads(resp.read().decode("utf-8"))
    lines = []
    for tr in doc.get("transcripts", []):
        for s in tr.get("sentences", []):
            spk = s.get("speaker_id")
            prefix = "说话人%d: " % (spk + 1) if spk is not None else ""
            lines.append(prefix + s.get("text", ""))
    if not lines:
        lines = [tr.get("text", "") for tr in doc.get("transcripts", [])]
    return "\n".join(x for x in lines if x)


def asr_post(wav_path):
    """把单个 wav 文件提交给 /audio/transcriptions,返回转写文本。"""
    boundary = "----BadgeBoundary%d" % int(time.time() * 1000)
    with open(wav_path, "rb") as f:
        audio = f.read()
    name = os.path.basename(wav_path)
    body = b"".join([
        ("--%s\r\n" % boundary).encode(),
        b'Content-Disposition: form-data; name="file"; filename="%s"\r\n' % name.encode(),
        b"Content-Type: audio/wav\r\n\r\n",
        audio,
        ("\r\n--%s\r\n" % boundary).encode(),
        b'Content-Disposition: form-data; name="model"\r\n\r\n',
        CONFIG["asr_model"].encode(),
        ("\r\n--%s--\r\n" % boundary).encode(),
    ])
    req = urllib.request.Request(
        CONFIG["asr_base_url"].rstrip("/") + "/audio/transcriptions",
        data=body,
        headers={
            "Content-Type": "multipart/form-data; boundary=%s" % boundary,
            "Authorization": "Bearer " + CONFIG["asr_api_key"],
        })
    with urllib.request.urlopen(req, timeout=600) as resp:
        result = json.loads(resp.read().decode("utf-8"))
    return result.get("text", "")


def asr_transcribe(wav_path):
    """语音转写入口:按 asr_provider 分流;未配 key 返回模拟转写文本。

    bailian: paraformer 文件转写,整段提交,支持数小时录音,带说话人分离。
    openai:  /audio/transcriptions,超长录音按 ASR_CHUNK_BYTES 切段拼接。
    """
    if not CONFIG["asr_api_key"]:
        size = os.path.getsize(wav_path)
        return "(模拟转写) 未配置 ASR API,后台已收到约 %.1f 秒的 PCM 录音。" % (size / 32000.0)
    if CONFIG.get("asr_provider") == "bailian":
        return bailian_transcribe(wav_path)
    if os.path.getsize(wav_path) - 44 <= ASR_CHUNK_BYTES:
        return asr_post(wav_path)
    texts = []
    with open(wav_path, "rb") as f:
        f.seek(44)   # 跳过 WAV 头
        index = 0
        while True:
            data = f.read(ASR_CHUNK_BYTES)
            if not data:
                break
            index += 1
            part = "%s.part%03d.wav" % (wav_path, index)
            with open(part, "wb") as g:
                write_wav_header(g, len(data))
                g.write(data)
            try:
                texts.append(asr_post(part))
            finally:
                os.unlink(part)
            print("[asr] %s 第 %d 段转写完成" % (os.path.basename(wav_path), index))
    return "\n".join(t for t in texts if t)


# 单次送给 LLM 的转写文本上限。超过就分段分析再汇总,
# 避免长会议只分析开头一部分。
# 百炼免费额度对有严格限速,单段 4000 字符实测约 137 秒;
# 段太长会撞上请求超时,故取 4000。提升额度后可适当调大。
LLM_CHUNK_CHARS = 4000

_ANALYZE_PROMPT_HEAD = (
    "你是资深的产品会议纪要助手。下面是一线人员沟通录音的语音转写文本,"
    "可能含有口音导致的同音/近音错词、重复、口语停顿,请完成两件事:\n\n"
    "第一步【找错】:找出明显的同音/近音误识别,尤其注意中英混读词"
    "(如 contact/context、记录/记忆等)。original 必须是原文中逐字复制的片段,"
    "便于程序精确替换;不要改口语重复。\n\n"
    "第二步【结构化】:基于理解后的内容输出 JSON,字段如下:\n"
    "  corrections: 纠错对照数组,每项 {original, corrected, reason};没有就空数组\n"
    "  summary: 一句话沟通摘要(中文,不超过80字)\n"
    "  sections: 分板块总结数组,每项 {title, points}。把沟通内容按主题归纳为若干板块,"
    "如\"项目框架梳理\"\"职业路径设计\"\"简历投递优化\"\"产品下架原因分析\"\"个人问题诊断\""
    "\"长期成长方向\"\"后续交付任务\"等,title 为板块名,points 为该板块下 1 到 4 条要点"
    "(中文,直接给结论,不要套话);没有明显板块就返回空数组\n"
    "  todos: 待办事项数组,每项 {title, assignee, due_days}。assignee 取\"用户\"\"对方\"\"双方\""
    "之一,表示这件事由谁来做(用户=工牌佩戴者本人,对方=沟通另一方,双方=需要两人协作)。"
    "仅当文本中出现明确的交付/行动项时才生成:如明确承诺、约定、要提交的材料、要跟进的事、"
    "指派任务、\"稍后/回头/下次\"要做的事。纯讨论、背景介绍、闲聊、描述现状、\"要思考\"\"要分析\""
    "\"要理解\"这类软性引导不算待办,没有就返回空数组,不要编造;同一类交付物(如简历、投递截图、"
    "对话截图)要拆成独立待办,不要合并。\n\n"
    "要求:只输出一个 JSON 对象,不要输出任何其他内容,不要复述原文全文。\n"
    "转写文本:\n"
)

_MERGE_PROMPT_HEAD = (
    "你是资深的产品会议纪要助手。下面是一场较长沟通按时间顺序分段分析后的结果。"
    "请把它们汇总成整场沟通的最终结论,只输出一个 JSON 对象,字段如下:\n"
    "  summary: 整场沟通一句话摘要(中文,不超过80字)\n"
    "  sections: 分板块总结数组,每项 {title, points}。合并各段相同主题,按内容归纳为若干板块"
    "(如\"项目框架梳理\"\"职业路径设计\"\"简历投递优化\"\"产品下架原因分析\"\"个人问题诊断\""
    "\"长期成长方向\"\"后续交付任务\"等),title 为板块名,points 为该板块下 1 到 4 条要点"
    "(中文,直接给结论);没有明显板块就返回空数组\n"
    "  todos: 待办事项数组,每项 {title, assignee, due_days}。assignee 取\"用户\"\"对方\"\"双方\""
    "之一。合并重复事项;仅保留确实存在的明确交付/行动项(明确承诺、要提交的材料、要跟进的事、"
    "指派任务等),\"要思考\"\"要分析\"\"要理解\"这类软性引导不算待办,没有就返回空数组,不要编造;"
    "同一类交付物(简历、投递截图、对话截图)拆成独立待办。\n\n"
    "要求:只输出一个 JSON 对象,不要任何其他内容。\n"
    "分段结果:\n"
)


_SALES_SCORE_PROMPT = (
    "你是资深的销售能力评估教练。下面是一段销售拜访客户的录音转写文本,"
    "已按说话人分离标记(说话人1/说话人2)。请完成三件事:\n\n"
    "第一件【角色判断】:判断哪一位是销售/工牌佩戴者(以下称说话人S),哪一位是客户(以下称说话人C)。"
    "判断依据:销售会主动介绍产品、问需求、提方案、推进下一步;客户会描述业务场景、提异议、"
    "表达需求、询问细节。输出 sales_voice_label,值为'说话人1'或'说话人2'。\n\n"
    "第二件【五维评分】:仅根据销售(说话人S)的表现,从以下五个维度各打一个 0-100 整数分,并给出 notes。"
    "五个维度及判断标准:\n"
    "  opening(开场白,权重10%):是否破冰、自我介绍、说明来意。0=完全没破冰;50=破冰了但没说来意;100=结构完整\n"
    "  product_intro(产品介绍,权重25%):核心卖点是否覆盖、是否结合客户痛点、是否说明版本更新。0=没介绍;50=覆盖但没结合痛点;100=精准匹配客户需求\n"
    "  need_discovery(痛点挖掘,权重25%):是否主动追问、是否下钻深层业务诉求、是否确认客户现状。0=只听客户说;50=问了表面;100=挖到业务根因\n"
    "  objection_handling(异议处理,权重20%):是否识别客户异议、回应是否有效、是否产生新异议。0=回避;50=回应了但没解决;100=客户表示认可\n"
    "  closing(收尾动作,权重20%):是否约定下一步、是否有明确时间、是否确认对接人、是否索要承诺。0=无明确收尾;50=泛泛说\"再联系\";100=有具体时间+动作\n\n"
    "第三件【亮点与漏项】:\n"
    "  missing_items:销售没做到的关键点,每项 {category, item, severity},"
    "category 取五个维度名之一,severity 取 high/medium/low,没有就空数组\n"
    "  strength_items:销售做得好的亮点,每项 {category, item, severity},"
    "severity 固定 positive,没有就空数组\n\n"
    "输出一个 JSON 对象,字段如下:\n"
    "  sales_voice_label: '说话人1' 或 '说话人2'\n"
    "  dimensions: {opening, product_intro, need_discovery, objection_handling, closing},"
    "每项 {score:int, notes:string}\n"
    "  missing_items: [...]\n"
    "  strength_items: [...]\n"
    "  overall: 综合分(整数,由各维度按权重加权求和)\n"
    "  overall_formula: 综合分计算公式字符串\n\n"
    "要求:只输出 JSON,不要任何其他内容,不要复述原文。\n"
    "转写文本:\n"
)


def _llm_chat(prompt):
    """调用 OpenAI 兼容 /chat/completions,返回解析后的 JSON 对象。"""
    payload = {
        "model": CONFIG["llm_model"],
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0.1,
    }
    req = urllib.request.Request(
        CONFIG["llm_base_url"].rstrip("/") + "/chat/completions",
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + CONFIG["llm_api_key"],
        })
    # 长文本分析耗时更久,超时放宽到 5 分钟。
    with urllib.request.urlopen(req, timeout=300) as resp:
        result = json.loads(resp.read().decode("utf-8"))
    content = result["choices"][0]["message"]["content"].strip()
    # 容错剥掉模型可能带的 ```json 代码围栏,再解析。
    content = re.sub(r"^```(?:json)?\s*|\s*```$", "", content, flags=re.MULTILINE)
    return json.loads(content)


def chunk_transcript(transcript, limit=LLM_CHUNK_CHARS):
    """按行切分转写文本,每段不超过 limit 字符,尽量落在说话人换行处。"""
    lines = transcript.splitlines(True)
    chunks = []
    buf = []
    size = 0
    for line in lines:
        if len(line) > limit:
            # 单行超长(极少见):先冲掉缓冲,再把这一行硬切
            if buf:
                chunks.append("".join(buf))
                buf, size = [], 0
            for i in range(0, len(line), limit):
                chunks.append(line[i:i + limit])
            continue
        if size + len(line) > limit and buf:
            chunks.append("".join(buf))
            buf, size = [], 0
        buf.append(line)
        size += len(line)
    if buf:
        chunks.append("".join(buf))
    return chunks or [""]


def _parse_sections(parsed):
    """从模型返回中解析分板块总结,并拍平出兼容用的关键信息列表。"""
    sections = []
    for s in (parsed.get("sections") or []):
        if not isinstance(s, dict):
            continue
        title = str(s.get("title") or "").strip()
        points = [str(x).strip() for x in (s.get("points") or []) if str(x).strip()]
        if title and points:
            sections.append({"title": title, "points": points})
    key_points = ["%s: %s" % (s["title"], p) for s in sections for p in s["points"]]
    return sections, key_points


def _parse_todos(parsed):
    """从模型返回中解析待办,统一兜底 due_days 与 assignee 的取值。"""
    todos = []
    for t in (parsed.get("todos") or []):
        if not isinstance(t, dict):
            continue
        title = str(t.get("title") or "").strip()
        if not title:
            continue
        assignee = str(t.get("assignee") or "").strip()
        if assignee not in ("用户", "对方", "双方"):
            assignee = ""
        todos.append({
            "title": title,
            "assignee": assignee,
            "due_days": int(t.get("due_days") or 3),
        })
    return todos


_SALES_DIMENSION_WEIGHTS = {
    "opening": 0.10,
    "product_intro": 0.25,
    "need_discovery": 0.25,
    "objection_handling": 0.20,
    "closing": 0.20,
}
_SALES_DIMENSION_NAMES = {
    "opening": "开场白",
    "product_intro": "产品介绍",
    "need_discovery": "痛点挖掘",
    "objection_handling": "异议处理",
    "closing": "收尾动作",
}


def _parse_sales_score(parsed, session_id, device):
    """从 LLM 返回中解析销售评分 JSON,补全权重并计算综合分。"""
    if not isinstance(parsed, dict):
        return None
    voice_label = str(parsed.get("sales_voice_label") or "").strip()
    if voice_label not in ("说话人1", "说话人2"):
        voice_label = ""
    raw_dims = parsed.get("dimensions") or {}
    dimensions = {}
    for key, weight in _SALES_DIMENSION_WEIGHTS.items():
        d = raw_dims.get(key) if isinstance(raw_dims, dict) else None
        if isinstance(d, dict):
            try:
                score = int(d.get("score", 0))
            except (ValueError, TypeError):
                score = 0
            score = max(0, min(100, score))
            notes = str(d.get("notes") or "").strip()
        else:
            score = 0
            notes = ""
        dimensions[key] = {
            "score": score,
            "weight": weight,
            "notes": notes,
        }
    overall = round(sum(dimensions[k]["score"] * dimensions[k]["weight"]
                        for k in _SALES_DIMENSION_WEIGHTS))
    def _parse_items(raw):
        out = []
        for item in raw or []:
            if not isinstance(item, dict):
                continue
            cat = str(item.get("category") or "").strip()
            item_text = str(item.get("item") or "").strip()
            sev = str(item.get("severity") or "").strip()
            if cat not in _SALES_DIMENSION_WEIGHTS:
                continue
            if not item_text:
                continue
            out.append({"category": cat, "item": item_text, "severity": sev})
        return out
    return {
        "version": 1,
        "session_id": session_id,
        "device": device,
        "sales_voice_label": voice_label,
        "overall": overall,
        "overall_formula": "开场白×10% + 产品介绍×25% + 痛点挖掘×25% + 异议处理×20% + 收尾动作×20%",
        "dimensions": dimensions,
        "missing_items": _parse_items(parsed.get("missing_items")),
        "strength_items": _parse_items(parsed.get("strength_items")),
    }


def llm_sales_score(corrected_transcript, session_id, device):
    """独立调用一次 LLM 产出销售五维评分。

    单独跑一次是因为评分需要看完整的销售发言,且不参与长文本分段流程。
    无 API key 时返回 None,上层 analysis 不带 sales_score 字段。
    """
    if not CONFIG["llm_api_key"] or not corrected_transcript.strip():
        return None
    try:
        parsed = _llm_chat(_SALES_SCORE_PROMPT + corrected_transcript)
    except Exception as e:
        print("[sales_score] LLM 调用失败: %s" % e)
        return None
    score = _parse_sales_score(parsed, session_id, device)
    if score:
        print("[sales_score] 综合分 %d, 说话人=%s" % (
            score["overall"], score["sales_voice_label"]))
    return score


def _llm_analyze_once(text):
    """单段转写文本的纠错与结构化分析。

    模型只返回纠错对照,纠错后全文由本地按对照替换原文得到,
    既避免长文本让模型复述全文导致超时,也保证文本忠实原文。
    """
    parsed = _llm_chat(_ANALYZE_PROMPT_HEAD + text)
    corrections = [
        {
            "original": str(c.get("original") or ""),
            "corrected": str(c.get("corrected") or ""),
            "reason": str(c.get("reason") or ""),
        }
        for c in (parsed.get("corrections") or []) if isinstance(c, dict)
    ]
    corrected = text
    for c in corrections:
        if c["original"] and c["original"] != c["corrected"]:
            corrected = corrected.replace(c["original"], c["corrected"])
    sections, key_points = _parse_sections(parsed)
    return {
        "summary": str(parsed.get("summary") or ""),
        "sections": sections,
        "key_points": key_points,
        "todos": _parse_todos(parsed),
        "corrected_transcript": corrected,
        "corrections": corrections,
    }


def _llm_merge(parts):
    """把多段分析结果汇总成整场沟通的分板块总结、关键信息与待办。"""
    lines = []
    for i, p in enumerate(parts, 1):
        lines.append("第%d段摘要: %s" % (i, p["summary"]))
        for s in p["sections"]:
            lines.append("第%d段[%s]: %s" % (i, s["title"], " / ".join(s["points"])))
        for t in p["todos"]:
            lines.append("第%d段待办: %s(%s,建议%d天内)" % (
                i, t["title"], t.get("assignee") or "未定", t["due_days"]))
    parsed = _llm_chat(_MERGE_PROMPT_HEAD + "\n".join(lines))
    sections, key_points = _parse_sections(parsed)
    return {
        "summary": str(parsed.get("summary") or ""),
        "sections": sections,
        "key_points": key_points,
        "todos": _parse_todos(parsed),
    }


def llm_analyze(transcript):
    """调用 OpenAI 兼容 /chat/completions 产出结构化沟通分析;未配 key 走模拟。

    返回 {summary, key_points[], todos:[{title, due_days}],
          corrected_transcript, corrections[]}。
    转写文本过长时分段分析再汇总,保证长会议全程都参与分析。
    """
    if not CONFIG["llm_api_key"]:
        return {
            "summary": "(模拟分析) 已收到沟通录音并完成归档。配置 LLM API 后,这里将输出"
                       "真实的沟通摘要、关键信息与待办事项。",
            "sections": [
                {"title": "演示说明", "points": [
                    "录音已完整接收并保存为 WAV",
                    "演示模式:模拟生成待办用于展示任务流程",
                    "可在 config.json 配置 asr/llm API 启用真实分析",
                ]},
            ],
            "key_points": [
                "录音已完整接收并保存为 WAV",
                "演示模式:模拟生成待办用于展示任务流程",
                "可在 config.json 配置 asr/llm API 启用真实分析",
            ],
            "todos": [
                {"title": "整理沟通要点并发送给客户", "assignee": "用户", "due_days": 1},
                {"title": "跟进沟通中约定的合作事项", "assignee": "双方", "due_days": 3},
            ],
            "corrected_transcript": "",
            "corrections": [],
        }
    chunks = chunk_transcript(transcript)
    if len(chunks) <= 1:
        return _llm_analyze_once(chunks[0])
    print("[analyze] 转写文本较长,分 %d 段分析后汇总" % len(chunks))
    parts = [_llm_analyze_once(c) for c in chunks]
    merged = _llm_merge(parts)
    # 纠错文本与纠错对照按段顺序拼接,保证全文完整。
    merged["corrected_transcript"] = "\n".join(
        p["corrected_transcript"] for p in parts if p["corrected_transcript"])
    merged["corrections"] = [c for p in parts for c in p["corrections"]]
    return merged


def analyze_recording(recording_id, device, wav_path):
    """分析流水线:转写 -> 结构化分析 -> 生成待确认任务 -> 飞书推送。

    在独立线程中执行,HTTP 响应不等待本函数。
    """
    mode = "replay" if "_replay" in recording_id else "stream"
    record = {
        "recording_id": recording_id,
        "device": device,
        "mode": mode,
        "bytes": os.path.getsize(wav_path),
        "seconds": round(os.path.getsize(wav_path) / 32000.0, 1),
        "created_at": time.strftime("%Y-%m-%d %H:%M:%S"),
        "status": "running",
        "transcript": "",
        "summary": "",
        "sections": [],
        "key_points": [],
        "todos": [],
        "task_ids": [],
    }
    save_analysis(record)
    try:
        record["transcript"] = asr_transcribe(wav_path)
    except (OSError, ValueError, KeyError) as e:
        record["status"] = "failed"
        record["summary"] = "转写失败: %s" % e
        save_analysis(record)
        print("[analyze] %s 转写失败: %s" % (recording_id, e))
        return
    try:
        analysis = llm_analyze(record["transcript"])
        record.update(analysis)
        record["status"] = "ok"
    except (OSError, ValueError, KeyError) as e:
        # LLM 失败不阻塞录音与识别文字的保存和推送,摘要标记原因。
        record["summary"] = "(沟通分析失败: %s)" % e
        record["status"] = "ok"
        print("[analyze] %s LLM 分析失败,仅推送转写: %s" % (recording_id, e))

    # 销售评分:单独调一次 LLM,失败不阻塞主流程。
    try:
        score = llm_sales_score(
            record.get("corrected_transcript") or record["transcript"],
            recording_id, device)
        if score:
            record["sales_score"] = score
    except Exception as e:
        print("[analyze] %s 销售评分异常,跳过: %s" % (recording_id, e))

    # 由待办生成任务:写入飞书多维表格,确认/指派在飞书内完成(卡片按钮或表格编辑)。
    record["task_records"] = []
    for todo in record["todos"]:
        due = (date.today() + timedelta(days=todo.get("due_days", 3))).isoformat()
        try:
            no, rec_id = base_create_task(todo["title"], todo.get("assignee", ""),
                                          due, recording_id)
        except (OSError, ValueError) as e:
            print("[analyze] 任务写入多维表格失败,跳过该条: %s" % e)
            continue
        record["task_ids"].append(no)
        record["task_records"].append({
            "no": no, "record_id": rec_id, "title": todo["title"],
            "assignee": todo.get("assignee", ""), "due": due})
    save_analysis(record)
    print("[analyze] %s 完成,生成 %d 条任务" % (recording_id, len(record["task_ids"])))

    # 飞书推送:纪要 + 录音 + 识别文字全文(未配置自建应用时退回纯文本)。
    feishu_push_recording(record, wav_path)


# --------------------------------------------------------------------------
# WAV 文件:先顺序写 PCM,收尾时回填头部长度字段
# --------------------------------------------------------------------------

def write_wav_header(f, data_bytes):
    """写 44 字节标准 PCM WAV 头;长度字段先占位,收尾时再回填。"""
    rate = 16000
    f.write(b"RIFF")
    f.write((36 + data_bytes).to_bytes(4, "little"))
    f.write(b"WAVEfmt ")
    f.write((16).to_bytes(4, "little"))
    f.write((1).to_bytes(2, "little"))           # PCM
    f.write((1).to_bytes(2, "little"))           # 单声道
    f.write(rate.to_bytes(4, "little"))
    f.write((rate * 2).to_bytes(4, "little"))    # 字节率 = 采样率 * 2 字节
    f.write((2).to_bytes(2, "little"))           # 块对齐
    f.write((16).to_bytes(2, "little"))          # 位深
    f.write(b"data")
    f.write(data_bytes.to_bytes(4, "little"))


# --------------------------------------------------------------------------
# 卡片按钮回调:飞书长连接接收「认领」点击,确认任务并把负责人记为点击人。
# 长连接由后台主动发起,局域网运行也能收到回调,不需要公网地址。
# --------------------------------------------------------------------------

# 群成员姓名缓存:{chat_id: {open_id: name}},避免每次回调都拉成员列表。
_CHAT_MEMBERS = {}


def _chat_member_name(chat_id, open_id):
    """从群成员列表查姓名(复用已有 IM 权限,无需通讯录权限)。"""
    if chat_id and chat_id not in _CHAT_MEMBERS:
        try:
            req = urllib.request.Request(
                "https://open.feishu.cn/open-apis/im/v1/chats/%s/members" % chat_id,
                headers={"Authorization": "Bearer " + feishu_token()})
            with urllib.request.urlopen(req, timeout=15) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            _CHAT_MEMBERS[chat_id] = {
                m.get("member_id"): m.get("name")
                for m in (data.get("data", {}).get("items") or [])}
        except (OSError, ValueError):
            _CHAT_MEMBERS[chat_id] = {}
    name = _CHAT_MEMBERS.get(chat_id, {}).get(open_id)
    return name or open_id[-6:]


def _cached_member_name(chat_id, open_id):
    """只读缓存里的群成员姓名,不发请求;未命中时退化为 open_id 末段。"""
    return _CHAT_MEMBERS.get(chat_id, {}).get(open_id) or open_id[-6:]


def warm_chat_members():
    """启动时预热群成员姓名缓存,保证卡片回调不需要实时发请求。"""
    try:
        _chat_member_name(feishu_chat_id() if feishu_app_ready() else "", "")
    except (OSError, ValueError):
        pass


def start_card_callback_listener():
    """启动飞书长连接监听卡片回传交互;阻塞运行,需在守护线程中启动。"""
    import copy
    import lark_oapi as lark
    from lark_oapi.event.callback.model.p2_card_action_trigger import (
        CallBackCard, CallBackToast, P2CardActionTriggerResponse)

    def _toast_resp(text, ok=True):
        # SDK 的 dict 解包对已构造对象有兼容问题,统一手动逐字段赋值。
        resp = P2CardActionTriggerResponse()
        resp.toast = CallBackToast()
        resp.toast.type = "success" if ok else "error"
        resp.toast.content = text
        return resp

    def on_card_action(data):
        try:
            value = data.event.action.value or {}
            act = str(value.get("act", ""))
            if act not in ("claim", "reject"):
                return None
            rid, no = str(value.get("rid", "")), str(value.get("no", ""))
            idx = str(value.get("idx", ""))
            title = str(value.get("title", ""))
            chat_id = (data.event.context.open_chat_id
                       if data.event.context else "") or ""
            mid = (data.event.context.open_message_id
                   if data.event.context else "") or ""
            # 只读预热好的姓名缓存,避免在回调里发 HTTP(飞书限 3 秒应答)。
            name = _cached_member_name(chat_id, data.event.operator.open_id)

            if act == "claim":
                result_text = "已被 %s 认领" % name
            else:
                result_text = "已被 %s 标记误提" % name

            # 同步只做本地卡片改写:立即应答,避免超时提示。
            resp = _toast_resp("已处理第 %s 条待办" % idx)
            payload = _CARD_STORE.get(mid)
            if payload:
                new_card = copy.deepcopy(payload)
                for el in new_card.get("elements", []):
                    if el.get("tag") != "column_set":
                        continue
                    hit = any(
                        ce.get("tag") == "button"
                        and str((ce.get("value") or {}).get("rid")) == rid
                        for col in el.get("columns", [])
                        for ce in col.get("elements", []))
                    if hit:
                        # 被操作的行去掉两个按钮,替换成结果文字,防止重复点击。
                        el["columns"] = [
                            el["columns"][0],
                            {"tag": "column", "width": "auto",
                             "vertical_align": "center",
                             "elements": [{"tag": "div", "text": {
                                 "tag": "lark_md",
                                 "content": "**%s**" % result_text}}]}]
                        break
                resp.card = CallBackCard()
                resp.card.type = "raw"
                resp.card.data = new_card
                _CARD_STORE[mid] = new_card

            # 写多维表格与群广播放到后台线程,不占用回调应答时间。
            def _work():
                try:
                    if act == "claim":
                        base_claim_task(rid, name)
                        print("[feishu] 任务 #%s(卡片第 %s 条)已被 %s 认领" % (no, idx, name))
                        feishu_send("待办 %s「%s」已由 %s 认领(确认并指派)" % (idx, title, name))
                    else:
                        # 误提:AI 提取错误,直接从任务库删除。
                        base_delete_task(rid)
                        print("[feishu] 任务 #%s(卡片第 %s 条)被 %s 标记误提并删除" % (no, idx, name))
                        feishu_send("待办 %s「%s」被 %s 标记为误提,已从任务库移除" % (idx, title, name))
                except (OSError, ValueError) as e:
                    print("[feishu] 任务 #%s 处理失败: %s" % (no, e))
            threading.Thread(target=_work, daemon=True).start()
            return resp
        except Exception as e:
            print("[feishu] 处理卡片回调失败: %s" % e)
            return _toast_resp("操作失败: %s" % e, ok=False)

    handler = (lark.EventDispatcherHandler.builder("", "")
               .register_p2_card_action_trigger(on_card_action)
               .build())
    client = lark.ws.Client(CONFIG["feishu_app_id"], CONFIG["feishu_app_secret"],
                            event_handler=handler, log_level=lark.LogLevel.WARNING)
    client.start()   # 阻塞;断线由 SDK 内部自动重连


# --------------------------------------------------------------------------
# HTTP 服务
# --------------------------------------------------------------------------

PAGE_CSS = """
body{font-family:"Microsoft YaHei",sans-serif;margin:0;background:#f4f6f9;color:#222}
header{background:#1f2d3d;color:#fff;padding:14px 24px;display:flex;gap:24px;align-items:baseline}
header h1{font-size:18px;margin:0}
header a{color:#9ecbff;text-decoration:none;font-size:14px}
main{max-width:960px;margin:20px auto;padding:0 16px}
table{width:100%;border-collapse:collapse;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.08)}
th,td{padding:8px 10px;border-bottom:1px solid #eee;font-size:14px;text-align:left;vertical-align:top}
th{background:#fafbfc}
input,select,button{font-size:14px;padding:4px 6px}
button{background:#2f6fed;color:#fff;border:0;border-radius:4px;cursor:pointer}
button.gray{background:#8895a7}
button.green{background:#2fa84f}
.card{background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.08);padding:14px 18px;margin-bottom:14px}
.card h3{margin:4px 0 8px;font-size:15px}
.meta{color:#7a8494;font-size:12px;margin-bottom:8px}
.tag{display:inline-block;padding:1px 8px;border-radius:10px;font-size:12px;color:#fff}
.tag.pending{background:#e6a23c}.tag.confirmed{background:#2f6fed}.tag.done{background:#2fa84f}
.tag.failed{background:#d9534f}.tag.running{background:#8895a7}
.kp{margin:4px 0 4px 18px;font-size:14px}
.bar{margin:12px 0}
.bar a{color:#2f6fed}
"""

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # ---- 基础输出 ----

    def send_json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            # 客户端(工牌)可能已断开,响应写不回去就算了。
            pass

    def send_html(self, html, code=200):
        body = html.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def redirect(self, to):
        self.send_response(303)
        self.send_header("Location", to)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def log_message(self, fmt, *args):
        print("[http] %s" % (fmt % args))

    # ---- 录音上传:POST /api/recordings/stream ----

    def read_chunked_body(self):
        """解析 chunked 请求体,返回 (bytes|None)。客户端中途断开返回 None。"""
        chunks = []
        try:
            while True:
                line = self.rfile.readline(64).strip()
                size = int(line.split(b";")[0], 16)
                if size == 0:
                    while True:   # 吃掉结尾的 trailer 头,直到空行
                        trailer = self.rfile.readline(64).strip()
                        if not trailer:
                            break
                    return b"".join(chunks)
                data = self.rfile.read(size)
                if data is None or len(data) < size:
                    return None
                chunks.append(data)
                self.rfile.read(2)   # 每块结尾的 CRLF
        except (OSError, ValueError):
            return None

    # 长会议录音(可达数小时、上百 MB)不能先读进内存,下面两个函数边收边写文件。

    def recv_chunked_to_file(self, f):
        """把 chunked 请求体直接写入文件,返回写入字节数;中途断开返回 None。"""
        total = 0
        try:
            while True:
                line = self.rfile.readline(64).strip()
                size = int(line.split(b";")[0], 16)
                if size == 0:
                    while True:   # 吃掉结尾的 trailer 头,直到空行
                        trailer = self.rfile.readline(64).strip()
                        if not trailer:
                            break
                    return total
                remaining = size
                while remaining > 0:
                    data = self.rfile.read(min(remaining, 65536))
                    if not data:
                        return None
                    f.write(data)
                    remaining -= len(data)
                    total += len(data)
                self.rfile.read(2)   # 每块结尾的 CRLF
        except (OSError, ValueError):
            return None

    def recv_fixed_to_file(self, f, length):
        """把 Content-Length 请求体直接写入文件,返回写入字节数;中途断开返回 None。"""
        total = 0
        try:
            while total < length:
                data = self.rfile.read(min(length - total, 65536))
                if not data:
                    return None
                f.write(data)
                total += len(data)
        except OSError:
            return None
        return total

    def read_body(self):
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            return self.read_chunked_body()
        length = int(self.headers.get("Content-Length", "0") or 0)
        try:
            body = self.rfile.read(length) if length else b""
            return body if length == 0 or len(body) == length else None
        except OSError:
            return None

    def handle_stream_upload(self, query):
        device = self.headers.get("X-Device-Id", "unknown").strip()
        rate = int(self.headers.get("X-Sample-Rate", "16000"))
        bits = int(self.headers.get("X-Sample-Bits", "16"))
        channels = int(self.headers.get("X-Channels", "1"))
        rec_mode = self.headers.get("X-Recording-Mode", "stream").strip()
        if rate != 16000 or bits != 16 or channels != 1:
            # 与固件约定仅支持 16k/16bit/单声道,参数不符直接拒绝。
            self.send_json({"error": "unsupported audio params"}, 400)
            return
        suffix = "_replay" if rec_mode == "replay" else ""
        recording_id = "%s_%s%s" % (device, time.strftime("%Y%m%d_%H%M%S"), suffix)
        wav_path = os.path.join(REC_DIR, recording_id + ".wav")
        # 边收边写:2 小时录音约 230 MB,整段读进内存会撑爆进程。
        with open(wav_path, "wb") as f:
            write_wav_header(f, 0)   # 长度字段先占位,收完再回填
            if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
                total = self.recv_chunked_to_file(f)
            else:
                length = int(self.headers.get("Content-Length", "0") or 0)
                total = self.recv_fixed_to_file(f, length) if length else 0
            if total:
                f.seek(0)
                write_wav_header(f, total)
        if not total:
            # 中途断开或空体:不完整,不处理。
            os.unlink(wav_path)
            print("[upload] %s 录音不完整,丢弃" % device)
            self.send_json({"ok": False, "error": "incomplete"}, 400)
            return
        print("[upload] %s 收到 %d 字节,存为 %s" % (device, total, recording_id))

        # 分析在后台线程执行,立即响应设备。
        threading.Thread(target=analyze_recording,
                         args=(recording_id, device, wav_path), daemon=True).start()
        self.send_json({"ok": True, "recording_id": recording_id})

    # ---- 设备任务拉取:GET /api/badge/tasks ----

    def handle_tasks_api(self, query):
        device = (query.get("device", [""])[0] or "").strip()
        self.send_json({"tasks": tasks_for_device(device)})

    # ---- 管理页:沟通记录(任务管理已迁至飞书多维表格,本地不再提供) ----

    def page_recordings(self):
        cards = []
        if os.path.isdir(ANA_DIR):
            names = sorted(os.listdir(ANA_DIR), reverse=True)
        else:
            names = []
        for name in names:
            record = load_analysis(name[:-5] if name.endswith(".json") else name)
            if not record:
                continue
            kps = "".join("<li class='kp'>%s</li>" % html_escape(k) for k in record.get("key_points", []))
            todos = "".join("<li class='kp'>%s%s(建议 %s 天内)</li>" % (
                ("[%s] " % t.get("assignee")) if t.get("assignee") else "",
                html_escape(t.get("title", "")), t.get("due_days", "?"))
                for t in record.get("todos", []))
            tasks_links = "、".join("#%s" % tid for tid in record.get("task_ids", [])) or "无"
            cards.append(
                "<div class='card'><h3>%s <span class='tag %s'>%s</span></h3>"
                "<div class='meta'>设备 %s | %s | %s | %s | 模式 %s</div>"
                "<p><b>摘要:</b>%s</p><b>关键信息:</b><ul>%s</ul>"
                "<b>待办事项:</b><ul>%s</ul><b>关联任务:</b>%s</div>" % (
                    html_escape(record["recording_id"]),
                    record["status"],
                    {"ok": "分析完成", "running": "分析中", "failed": "分析失败"}.get(
                        record["status"], record["status"]),
                    html_escape(record.get("device", "")),
                    fmt_duration(record.get("seconds") or 0),
                    record.get("created_at", ""),
                    "%.1f KB" % (record.get("bytes", 0) / 1024.0),
                    record.get("mode", "stream"),
                    html_escape(record.get("summary", "")) or "-",
                    kps or "<li class='kp'>-</li>",
                    todos or "<li class='kp'>-</li>",
                    tasks_links))
        html = """<!DOCTYPE html><html lang="zh"><head><meta charset="utf-8">
<title>数字工牌 - 沟通记录</title><style>%s</style></head><body>
<header><h1>数字工牌后台</h1><a href="/recordings">沟通记录</a></header>
<main>%s</main></body></html>""" % (PAGE_CSS, "\n".join(cards) or "<div class='card'>暂无记录</div>")
        self.send_html(html)

    # ---- 路由 ----

    def do_GET(self):
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        if parsed.path == "/api/badge/tasks":
            self.handle_tasks_api(query)
        elif parsed.path in ("/", "/recordings"):
            self.page_recordings()
        else:
            self.send_json({"error": "not found"}, 404)

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path == "/api/recordings/stream":
            self.handle_stream_upload(parse_qs(parsed.query))
            return
        if parsed.path == "/api/debug/test_card":
            # 调试入口:建 4 条测试待办并推送纪要卡片,验证认领/误提交互。
            todos = [
                ("[测试] 整理客户对中东航线舱位保障的三点顾虑并发给客户", "我", "2026-10-29"),
                ("[测试] 向操作团队确认杰贝阿里港本周现舱情况", "我", "2026-10-29"),
                ("[测试] 客户提供现有供应商的合作价格区间", "客户", "2026-10-31"),
                ("[测试] 约定下周三上门演示 SOP 与舱位保障方案", "双方", "2026-11-02"),
            ]
            task_records = []
            for title, assignee, due in todos:
                no, rid = base_create_task(title, assignee, due, "TESTCARD")
                task_records.append({"no": no, "title": clip_title(title),
                                     "assignee": assignee, "due": due,
                                     "record_id": rid})
            _send_summary_card({
                "device": "TESTCARD", "recording_id": "TESTCARD",
                "seconds": 245, "created_at": time.strftime("%Y-%m-%d %H:%M:%S"),
                "summary": "[多任务卡片测试] 验证编号、认领置灰与误提删除。",
                "sections": [{"title": "测试说明", "points": [
                    "点「认领」:任务在多维表格置为已确认,按钮变成已认领文字",
                    "点「误提」:任务从多维表格删除,按钮变成已标记误提文字",
                    "测试完成后在多维表格里删除剩余 [测试] 任务即可"]}],
                "task_records": task_records})
            self.send_json({"ok": True})
            return
        self.send_json({"error": "not found"}, 404)


def html_escape(text):
    return (str(text).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


class BadgeServer(ThreadingHTTPServer):
    # 工牌随时可能断开(录音取消/信号差),连接类异常只记一行,不打整段栈。
    def handle_error(self, request, client_address):
        import sys
        exc = sys.exc_info()[1]
        if isinstance(exc, (ConnectionAbortedError, ConnectionResetError, BrokenPipeError)):
            print("[http] 客户端 %s 断开连接" % (client_address,))
        else:
            super().handle_error(request, client_address)


def main():
    # Windows 控制台默认 GBK,强制 UTF-8 输出避免日志中文乱码。
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")

    global CONFIG
    if os.path.exists(CONFIG_PATH):
        try:
            with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                user_cfg = json.load(f)
            CONFIG.update({k: v for k, v in user_cfg.items() if k in DEFAULT_CONFIG})
        except (OSError, ValueError) as e:
            print("[config] 读取 config.json 失败,使用默认配置: %s" % e)
    else:
        # 首次运行生成一份带注释说明的配置模板。
        with open(CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(DEFAULT_CONFIG, f, ensure_ascii=False, indent=2)
        print("[config] 已生成配置模板 config.json,填入飞书 Webhook 与 ASR/LLM 密钥后重启生效")

    # 密钥回退:config.json 未填时读环境变量 DASHSCOPE_API_KEY。
    dashscope_key = os.environ.get("DASHSCOPE_API_KEY", "")
    if not CONFIG["asr_api_key"]:
        CONFIG["asr_api_key"] = dashscope_key
    if not CONFIG["llm_api_key"]:
        CONFIG["llm_api_key"] = dashscope_key
    # 百炼模式下的端点与模型默认:LLM 走 DashScope 兼容端点 + deepseek-v4.1-flash。
    if CONFIG.get("asr_provider") == "bailian":
        if CONFIG["llm_base_url"] == DEFAULT_CONFIG["llm_base_url"]:
            CONFIG["llm_base_url"] = DASHSCOPE_ROOT + "/compatible-mode/v1"
        if CONFIG["llm_model"] in (DEFAULT_CONFIG["llm_model"], ""):
            # qwen-plus/turbo 在某些账号下 403 AllocationQuota.FreeTierOnly。
            # 实测长文本(4000 字符)deepseek-v4.1-flash 约 137 秒可返回,
            # glm-5.3 超过 300 秒超时,故默认用 deepseek。
            CONFIG["llm_model"] = "deepseek-v4.1-flash"

    for d in (REC_DIR, ANA_DIR):
        os.makedirs(d, exist_ok=True)

    # 任务库在飞书多维表格:启动卡片「认领」按钮的长连接回调监听。
    if base_ready():
        # 预热群成员姓名,卡片回调里只读缓存不再发请求。
        warm_chat_members()
        threading.Thread(target=start_card_callback_listener, daemon=True).start()
        print("[feishu] 任务库已连接多维表格,卡片认领回调监听已启动")
    else:
        print("[feishu] 未配置多维表格,待办将不会生成任务")

    addr = (CONFIG["host"], int(CONFIG["port"]))
    server = BadgeServer(addr, Handler)
    mode = "真实" if CONFIG["asr_api_key"] and CONFIG["llm_api_key"] else "模拟"
    feishu_mode = ("自建应用" if feishu_app_ready()
                   else ("Webhook" if CONFIG["feishu_webhook"] else "未配置"))
    print("数字工牌演示后台已启动: http://0.0.0.0:%s (分析模式: %s, 转写: %s, 飞书: %s)" % (
        CONFIG["port"], mode, CONFIG.get("asr_provider"), feishu_mode))
    print("  录音上传:  POST /api/recordings/stream")
    print("  任务拉取:  GET  /api/badge/tasks?device=<设备ID>")
    print("  任务管理:  飞书多维表格(群卡片「认领」按钮或直接编辑表格)")
    print("  沟通记录:  http://<本机IP>:%s/recordings" % CONFIG["port"])
    server.serve_forever()


if __name__ == "__main__":
    main()
