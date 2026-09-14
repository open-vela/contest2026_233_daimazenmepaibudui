这是一个占位文件，用来保证 /etc/assets 目录在仓库里存在。

放演示素材（提示音、图片、模型、配置文件等任何只读数据）的方法：
    把文件直接拷进  board/contest_board/src/etc/assets/
然后重新编译 + 烧录即可。

上板后它们出现在：  /etc/assets/<你的文件名>
权限是只读（-r--r--r--），因为整个 /etc 是一个 ROMFS 镜像，
编译时被打进固件里了（genromfs → romfs_etc.c → 链接进 nuttx.bin）。


两个特殊文件（都是本机生成、**不进版本库**，见 .git/info/exclude）：

  push_key.txt        手机推送的 device key，运行时被 network_comm.c 读走。
  agent_config.json   ai_agent 的凭据（大模型 key、火山 ASR/TTS 凭据）。
                      开机时由板级 sf32lb52_install_agent_config() 拷到
                      /data/ai_agent/config/config.json —— 因为 /data 是 tmpfs，
                      重启就清空，不拷的话每次都要手敲 set_llm / set_volc_*。
                      没有这个文件也能正常开机（跳过即可，固件里不含任何密钥）。
详见 docs/rom_assets_usage.md
