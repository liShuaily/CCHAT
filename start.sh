#!/bin/bash

echo "🧹 [1/4] 启动战前清场协议..."
# 1. 精准狙击 3000 端口（绝对不碰 VS Code 的 Node 进程）
fuser -k 3000/tcp 2>/dev/null
# 2. 击杀旧的 C++ 核心
killall -9 server 2>/dev/null
echo "✅ 清场完毕！"

echo "🔨 [2/4] 正在编译 C++ 核心引擎..."
cd ~/CCHAT/build
make -j4

echo "🚀 [3/4] 正在启动 C++ 后厨核心 (后台运行)..."
./server &
# 稍微等 2 秒，确保 C++ 核心完全启动并绑定 50051 端口
sleep 2 

echo "✨ [4/4] 正在启动 Node.js 网关大堂经理 (后台运行)..."
cd ~/CCHAT
node gateway.js &

echo "================================================="
echo "🎉 CCHAT 3.0 多媒体战术网络已全线点亮！"
echo "================================================="
echo "请在下方单独运行 cpolar 开启穿透："
echo "cpolar http 3000"
echo "================================================="