这是一个占位文件，用来保证 /etc/assets 目录在仓库里存在。

放演示素材（提示音、图片、模型、配置文件等任何只读数据）的方法：
    把文件直接拷进  board/contest_board/src/etc/assets/
然后重新编译 + 烧录即可。

上板后它们出现在：  /etc/assets/<你的文件名>
权限是只读（-r--r--r--），因为整个 /etc 是一个 ROMFS 镜像，
编译时被打进固件里了（genromfs → romfs_etc.c → 链接进 nuttx.bin）。

详见 docs/rom_assets_usage.md
