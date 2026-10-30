@echo off
chcp 65001 >nul
cd /d "%~dp0"

rem 首次运行用示例配置生成 config.json,再按需填入百炼密钥与飞书凭据。
if not exist config.json (
  copy config.example.json config.json >nul
  echo 已生成 config.json,请填入 asr_api_key / llm_api_key(或设置环境变量 DASHSCOPE_API_KEY)。
)

rem 打印本机局域网 IP,配置工牌时填这个地址加端口。
echo 本机可用的局域网地址(工牌配置页里填 http://这里的IP:8000):
python -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);s.connect(('8.8.8.8',80));print('  http://%s:8000' % s.getsockname()[0])"
echo.

python -u badge_server.py
pause
