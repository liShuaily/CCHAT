// 🌟 永久免疫代理劫持补丁 🌟
process.env.http_proxy = '';
process.env.https_proxy = '';
process.env.all_proxy = '';
process.env.no_proxy = 'localhost,127.0.0.1,::1';

const express = require('express');
const app = express();
const http = require('http').createServer(app);
const io = require('socket.io')(http, { cors: { origin: "*", methods: ["GET", "POST"] }});
const grpc = require('@grpc/grpc-js');
const protoLoader = require('@grpc/proto-loader');

const multer = require('multer');
const path = require('path');
const fs = require('fs');

const uploadDir = path.join(__dirname, 'uploads');
if (!fs.existsSync(uploadDir)) {
    fs.mkdirSync(uploadDir, { recursive: true });
}

const storage = multer.diskStorage({
    destination: (req, file, cb) => { cb(null, uploadDir); },
    filename: (req, file, cb) => {
        const uniqueSuffix = Date.now() + '-' + Math.round(Math.random() * 1E9);
        cb(null, uniqueSuffix + path.extname(file.originalname));
    }
});
const upload = multer({ storage: storage, limits: { fileSize: 50 * 1024 * 1024 } }); 

app.use('/uploads', express.static(uploadDir));
app.post('/api/upload', upload.single('mediaFile'), (req, res) => {
    if (!req.file) return res.status(400).json({ error: '没有收到文件' });
    const fileUrl = `/uploads/${req.file.filename}`;
    const fileType = req.file.mimetype;
    res.json({ url: fileUrl, type: fileType });
});

const packageDef = protoLoader.loadSync('./proto/chat.proto', { keepCase: true });
const chatProto = grpc.loadPackageDefinition(packageDef).chat;
const grpcClient = new chatProto.ChatService('127.0.0.1:50051', grpc.credentials.createInsecure());

app.get('/', (req, res) => {
  res.send(`
    <!DOCTYPE html>
    <html lang="zh">
    <head>
        <meta charset="UTF-8">
        <title>CCHAT 4.0 (AI Agent 全能版)</title>
        <script src="https://cdn.tailwindcss.com"></script>
        <style>
            body { margin: 0; font-family: 'Segoe UI', Tahoma, sans-serif; background: #0f172a; color: #e2e8f0; height: 100vh; overflow: hidden; display: flex; }
            #auth-modal { position: absolute; top:0; left:0; width: 100%; height: 100%; background: rgba(0,0,0,0.7); display: flex; justify-content: center; align-items: center; z-index: 100; backdrop-filter: blur(10px); }
            .auth-box { background: #1e293b; padding: 40px 30px; border-radius: 16px; border: 1px solid #334155; width: 320px; text-align: center; box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.5); }
            .auth-box h2 { color: #38bdf8; font-weight: bold; margin-bottom: 20px; font-size: 24px; }
            .auth-box input { width: 90%; padding: 12px; margin: 10px 0; background: #0f172a; border: 1px solid #334155; border-radius: 8px; color: white; outline: none; transition: 0.3s; }
            .auth-box input:focus { border-color: #38bdf8; box-shadow: 0 0 0 2px rgba(56,189,248,0.2); }
            .auth-box button { width: 100%; padding: 12px; margin-top: 15px; background: #0ea5e9; color: white; border: none; border-radius: 8px; font-weight: bold; cursor: pointer; transition: 0.3s; }
            .auth-box button:hover { background: #0284c7; }
            .auth-toggle { margin-top: 15px; color: #94a3b8; cursor: pointer; font-size: 14px; transition: 0.3s;}
            .auth-toggle:hover { color: #38bdf8; }
            
            #app-container { display: none; width: 100%; max-width: 1400px; margin: 0 auto; background: #1e293b; box-shadow: 0 0 30px rgba(0,0,0,0.5); border-left: 1px solid #334155; border-right: 1px solid #334155; }
            
            #sidebar { width: 300px; background: #0f172a; border-right: 1px solid #334155; display: flex; flex-direction: column; }
            #my-profile { padding: 20px; background: #0ea5e9; color: white; font-weight: bold; font-size: 18px; display: flex; align-items: center; justify-content: space-between; gap: 10px; }
            .logout-btn { font-size: 12px; font-weight: normal; cursor: pointer; padding: 4px 8px; background: rgba(0,0,0,0.2); border-radius: 4px; transition: 0.2s; }
            .logout-btn:hover { background: rgba(0,0,0,0.5); }
            
            .section-title { padding: 10px 20px; font-size: 12px; font-weight: bold; color: #64748b; text-transform: uppercase; letter-spacing: 1px; }
            
            .contact-list { flex: 1; overflow-y: auto; padding: 0 10px; }
            .contact-item { padding: 12px 15px; border-radius: 12px; margin-bottom: 5px; cursor: pointer; transition: 0.2s; display: flex; align-items: center; gap: 10px; position: relative; color: #cbd5e1;}
            .contact-item:hover { background: #1e293b; color: white;}
            .contact-item.active { background: #38bdf8; color: #0f172a; font-weight: bold; }
            
            .sidebar-del-btn { display: none; color: #ef4444; font-size: 12px; cursor: pointer; font-weight: normal; position: absolute; right: 15px;}
            .contact-item:hover .sidebar-del-btn { display: inline-block; }
            .sidebar-del-btn:hover { text-decoration: underline; }
            
            .unread-badge { background: #ef4444; color: white; border-radius: 12px; padding: 2px 7px; font-size: 11px; font-weight: bold; position: absolute; right: 15px; box-shadow: 0 2px 4px rgba(239,68,68,0.4);}
            
            #add-friend-box { display: flex; padding: 15px; border-top: 1px solid #334155; background: #0f172a;}
            #add-friend-box input { flex: 1; padding: 10px; background: #1e293b; border: 1px solid #334155; color: white; border-radius: 8px; outline:none; font-size: 13px;}
            #add-friend-box button { background: #0ea5e9; color: white; border: none; border-radius: 8px; margin-left: 8px; padding: 0 15px; cursor: pointer; font-size: 13px;}
            
            #chat-area { flex: 1; display: flex; flex-direction: column; background: #1e293b; position: relative;}
            #chat-header { padding: 20px; border-bottom: 1px solid #334155; font-size: 18px; font-weight: bold; display: flex; justify-content: space-between; align-items: center; color: white;}
            .clear-chat-btn { font-size: 13px; font-weight: normal; color: #94a3b8; cursor: pointer; padding: 6px 12px; border-radius: 6px; background: #334155; transition: 0.2s;}
            .clear-chat-btn:hover { background: #ef4444; color: white; }
            
            #messages { flex: 1; padding: 20px; overflow-y: auto; margin: 0; list-style: none; background: #1e293b; }
            .time-divider { text-align: center; font-size: 12px; color: #64748b; margin: 20px 0 15px 0; }
            .msg-row { display: flex; margin-bottom: 20px; flex-direction: column; }
            .msg-row.me { align-items: flex-end; }
            .msg-row.other { align-items: flex-start; }
            .msg-content-wrapper { display: flex; max-width: 80%; align-items: flex-end; gap: 10px; }
            .me .msg-content-wrapper { flex-direction: row-reverse; }
            .avatar-img { width: 40px; height: 40px; border-radius: 50%; object-fit: cover; background: #334155; border: 2px solid #475569; }
            .msg-bubble { padding: 12px 16px; border-radius: 16px; word-wrap: break-word; line-height: 1.5; position: relative; font-size: 15px;}
            .me .msg-bubble { background: #0ea5e9; color: white; border-bottom-right-radius: 4px; box-shadow: 0 4px 6px -1px rgba(14, 165, 233, 0.3); }
            .other .msg-bubble { background: #334155; color: #f8fafc; border-bottom-left-radius: 4px; box-shadow: 0 4px 6px -1px rgba(0, 0, 0, 0.2); }
            .msg-system { text-align: center; color: #64748b; font-size: 13px; margin: 10px auto; background: #0f172a; padding: 4px 12px; border-radius: 12px;}
            .msg-actions { font-size: 12px; color: #64748b; margin-top: 6px; display: flex; gap: 10px; opacity: 0; transition: opacity 0.2s; }
            .msg-row:hover .msg-actions { opacity: 1; }
            .action-link { cursor: pointer; color: #38bdf8; }
            .action-link:hover { text-decoration: underline; }
            
            #input-area { display: flex; padding: 15px 20px; background: #0f172a; border-top: 1px solid #334155; align-items: center; gap: 12px;}
            #input-area input[type="text"] { flex: 1; padding: 14px 20px; background: #1e293b; border: 1px solid #475569; color: white; border-radius: 24px; outline: none; font-size: 15px; transition: 0.2s;}
            #input-area input[type="text"]:focus { border-color: #38bdf8; }
            #input-area button.send-btn { background: #0ea5e9; color: white; border: none; padding: 14px 28px; border-radius: 24px; cursor: pointer; font-weight: bold; transition: 0.2s;}
            #input-area button.send-btn:hover { background: #0284c7; }
            
            input[type="file"] { display: none; }
            .tool-btn { cursor: pointer; color: #94a3b8; transition: 0.2s; display: flex; align-items: center; justify-content: center; width: 40px; height: 40px; border-radius: 50%; background: #1e293b;}
            .tool-btn:hover { color: #38bdf8; background: #334155;}

            #emoji-picker { display: none; position: absolute; bottom: 80px; left: 20px; background: #1e293b; border: 1px solid #334155; border-radius: 12px; padding: 10px; width: 260px; box-shadow: 0 10px 25px rgba(0,0,0,0.5); z-index: 50; grid-template-columns: repeat(6, 1fr); gap: 5px; max-height: 200px; overflow-y: auto;}
            .emoji-item { text-align: center; cursor: pointer; font-size: 20px; padding: 5px; border-radius: 6px; transition: 0.2s;}
            .emoji-item:hover { background: #334155; transform: scale(1.2);}

            .streaming-cursor { display: inline-block; width: 6px; height: 16px; background-color: #38bdf8; vertical-align: middle; animation: blink 0.8s step-end infinite; margin-left: 4px; }
            @keyframes blink { 50% { opacity: 0; } }
        </style>
    </head>
    <body>

    <div id="auth-modal">
        <div class="auth-box" id="login-box">
            <h2>CCHAT 安全接入</h2>
            <input type="text" id="l-user" placeholder="微信号 (不可更改)" autocomplete="off" />
            <input type="password" id="l-pass" placeholder="访问密钥" />
            <button id="btn-login">登 录</button>
            <div class="auth-toggle" onclick="toggleAuth()">未注册？点此申请权限</div>
        </div>
        <div class="auth-box" id="reg-box" style="display:none;">
            <h2>注册特工档案</h2>
            <input type="text" id="r-user" placeholder="微信号 (不可更改)" autocomplete="off" />
            <input type="password" id="r-pass" placeholder="设定密钥" />
            <button id="btn-reg">注 册</button>
            <div class="auth-toggle" onclick="toggleAuth()">已有档案？返回登录</div>
        </div>
    </div>

    <div id="app-container">
        <div id="sidebar">
            <div id="my-profile">
                <div style="display:flex; align-items:center; gap:10px;">
                    <img id="my-avatar" src="" class="avatar-img" style="width:36px; height:36px;" />
                    <span id="my-name-display">加载中...</span>
                </div>
                <div class="logout-btn" onclick="manualLogout()">注销</div>
            </div>
            <div class="section-title">新朋友申请</div>
            <div id="pending-panel" style="padding: 0 10px;"></div>
            <div class="section-title">加密链路</div>
            <div class="contact-list" id="contact-panel"></div>
            <div id="add-friend-box">
                <input type="text" id="new-friend-input" placeholder="输入对方微信号...">
                <button onclick="sendFriendReq()">添加</button>
            </div>
        </div>
        
        <div id="chat-area">
            <div id="chat-header">
                <div style="display:flex; align-items:center; gap:10px;">
                    <img id="chat-title-avatar" src="" class="avatar-img" style="width:36px; height:36px; border-radius:8px;" />
                    <span id="chat-title">公共大厅</span>
                </div>
                <div class="clear-chat-btn" onclick="clearActiveChat()">🗑️ 清空本地记录</div>
            </div>
            <ul id="messages"></ul>
            
            <div id="emoji-picker"></div>

            <div id="input-area">
                <div class="tool-btn" id="btn-emoji" title="发送表情">😀</div>
                <label for="fileInput" class="tool-btn" title="发送图片/视频">
                    <svg class="w-6 h-6" fill="none" stroke="currentColor" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M15.172 7l-6.586 6.586a2 2 0 102.828 2.828l6.414-6.586a4 4 0 00-5.656-5.656l-6.415 6.585a6 6 0 108.486 8.486L20.5 13"></path></svg>
                </label>
                <input type="file" id="fileInput" accept="image/*,video/*,audio/*" />
                <input type="text" id="chat-input" placeholder="输入战术指令或聊天内容 (支持指派AI操作)..." autocomplete="off"/>
                <button class="send-btn" onclick="sendMsg()">发 送</button>
            </div>
        </div>
    </div>

    <script src="/socket.io/socket.io.js"></script>
    <script>
        const socket = io();
        let isLoginMode = true; let myName = "";
        let activeChat = "ALL"; let allMessages = []; 
        let unreadCounts = {};

        function getAvatarUrl(username) {
            if(username === 'ALL') return "https://api.dicebear.com/7.x/shapes/svg?seed=大厅";
            if(username === '🤖 AI 管家') return "https://api.dicebear.com/7.x/bottts/svg?seed=AI管家&backgroundColor=0ea5e9";
            return \`https://api.dicebear.com/7.x/adventurer/svg?seed=\${encodeURIComponent(username)}&backgroundColor=334155\`;
        }

        const emojis = ["😀","😂","🤣","😊","🥰","😍","😎","😋","😏","🙄","🤔","🤐","😫","😴","😛","😜","🤤","😓","😔","😕","🙃","🤑","😲","☹️","🙁","😖","😞","😟","😤","😢","😭","😦","😧","😨","😩","🤯","😬","😰","😱","🥵","🥶","😳","🤪","😵","😡","😠","🤬","😷","🤒","🤕","🤢","🤮","🤧","😇","🥳","🥺","🤠","🤡","🤥","🤫","🤭","🧐","🤓","😈","👿","👹","👺","💀","👻","👽","👾","🤖","🎃","😺","😸","😹","😻","😼","😽","🙀","😿","😾"];
        const emojiPicker = document.getElementById('emoji-picker');
        emojis.forEach(e => {
            let span = document.createElement('div');
            span.className = 'emoji-item'; span.innerText = e;
            span.onclick = () => { const input = document.getElementById('chat-input'); input.value += e; input.focus(); };
            emojiPicker.appendChild(span);
        });
        document.getElementById('btn-emoji').onclick = (e) => { e.stopPropagation(); emojiPicker.style.display = emojiPicker.style.display === 'grid' ? 'none' : 'grid'; };
        document.body.onclick = () => { emojiPicker.style.display = 'none'; };
        document.getElementById('emoji-picker').onclick = (e) => e.stopPropagation();

        function getLocalTimeString() {
            const d = new Date(); const pad = (n) => n.toString().padStart(2, '0');
            return d.getFullYear() + "-" + pad(d.getMonth()+1) + "-" + pad(d.getDate()) + " " + pad(d.getHours()) + ":" + pad(d.getMinutes()) + ":" + pad(d.getSeconds());
        }

        function uuidv4() { return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => { var r = Math.random() * 16 | 0, v = c == 'x' ? r : (r & 0x3 | 0x8); return v.toString(16); }); }
        function toggleAuth() { isLoginMode = !isLoginMode; document.getElementById('login-box').style.display = isLoginMode ? 'block' : 'none'; document.getElementById('reg-box').style.display = isLoginMode ? 'none' : 'block'; }

        document.getElementById('btn-reg').onclick = () => { const u = document.getElementById('r-user').value.trim(); const p = document.getElementById('r-pass').value.trim(); if(u && p) socket.emit('register', {username: u, password: p}); };
        socket.on('register_res', (res) => { alert(res.message); if(res.success) toggleAuth(); });

        document.getElementById('btn-login').onclick = () => { const u = document.getElementById('l-user').value.trim(); const p = document.getElementById('l-pass').value.trim(); if(u && p) { myName = u; socket.emit('login', {username: u, password: p}); } };

        socket.on('login_res', (res) => {
            if(res.success) {
                document.getElementById('auth-modal').style.display = 'none';
                document.getElementById('app-container').style.display = 'flex';
                document.getElementById('my-name-display').innerText = res.nickname || myName;
                document.getElementById('my-avatar').src = res.avatar_url || getAvatarUrl(myName);
                document.getElementById('chat-title-avatar').src = getAvatarUrl('ALL');
                
                socket.emit('get pending', { username: myName }); socket.emit('get friends', { username: myName }); socket.emit('sync messages', { username: myName });
            } else alert(res.message);
        });

        function manualLogout() { location.reload(); }

        // 🌟 响应 AI 管家的系统级注销指令
        socket.on('force_logout_client', () => {
            alert("AI 已根据您的指令执行系统注销。");
            location.reload();
        });

        function updateBadges() {
            const allBadge = document.getElementById('badge-ALL');
            if(allBadge) { allBadge.innerText = unreadCounts['ALL'] || ''; allBadge.style.display = unreadCounts['ALL'] ? 'inline-block' : 'none'; }
            document.querySelectorAll('.friend-badge').forEach(el => {
                const f = el.getAttribute('data-friend');
                el.innerText = unreadCounts[f] || ''; el.style.display = unreadCounts[f] ? 'inline-block' : 'none';
            });
        }

        function switchChat(target) {
            activeChat = target;
            unreadCounts[target] = 0;
            updateBadges();

            document.querySelectorAll('.contact-item').forEach(el => el.classList.remove('active'));
            const targetEl = document.getElementById('contact-' + target);
            if(targetEl) targetEl.classList.add('active');
            
            document.getElementById('chat-title').innerText = target === 'ALL' ? "公共大厅" : "与 " + target + " 加密通信";
            document.getElementById('chat-title-avatar').src = getAvatarUrl(target);

            renderMessages();
        }

        function sendFriendReq() { const f = document.getElementById('new-friend-input').value.trim(); if(f && f !== myName) socket.emit('send request', { me: myName, target: f }); document.getElementById('new-friend-input').value = ''; }
        socket.on('req_res', (res) => { alert(res.message); });

        socket.on('pending_res', (users) => {
            const panel = document.getElementById('pending-panel'); panel.innerHTML = '';
            if (!users || users.length === 0) { panel.innerHTML = '<div style="color:#64748b; font-size:12px; padding:10px; text-align:center;">暂无新申请</div>'; return; }
            users.forEach(u => {
                if(!u || !u.account_id) return;
                let reqId = u.account_id; let reqName = u.nickname ? u.nickname : reqId;
                const div = document.createElement('div');
                div.style.cssText = "display:flex; align-items:center; justify-content:space-between; background:#1e293b; padding:10px; border-radius:8px; margin-bottom:8px; border: 1px solid #334155;";
                div.innerHTML = \`
                    <div style="display:flex; align-items:center; gap:10px;">
                        <img src="\${u.avatar_url || getAvatarUrl(reqId)}" class="avatar-img" style="width:32px; height:32px; border-radius:50%;" />
                        <span style="color:#f8fafc; font-size:14px; font-weight:bold;" title="特工代号: \${reqId}">\${reqName}</span>
                    </div>
                    <div style="display:flex; gap:6px;">
                        <button onclick="handleReq('\${reqId}', true)" style="background:#10b981; color:white; border:none; padding:6px 12px; border-radius:6px; cursor:pointer; font-size:12px; font-weight:bold;">同意</button>
                        <button onclick="handleReq('\${reqId}', false)" style="background:#ef4444; color:white; border:none; padding:6px 12px; border-radius:6px; cursor:pointer; font-size:12px; font-weight:bold;">拒绝</button>
                    </div>
                \`;
                panel.appendChild(div);
            });
        });

        function handleReq(sender, accept) { socket.emit('handle request', { me: myName, sender: sender, accept: accept }); }
        socket.on('handle_res', () => { socket.emit('get pending', { username: myName }); socket.emit('get friends', { username: myName }); });

        socket.on('friends_res', (friends) => {
            const panel = document.getElementById('contact-panel');
            panel.innerHTML = \`
                <div class="contact-item \${activeChat === 'ALL' ? 'active' : ''}" onclick="switchChat('ALL')" id="contact-ALL">
                    <img src="\${getAvatarUrl('ALL')}" class="avatar-img" style="width:30px; height:30px; border-radius:8px;" />
                    <span>公共大厅 <span id="badge-ALL" class="unread-badge" style="display:none"></span></span>
                </div>
                <div class="contact-item \${activeChat === '🤖 AI 管家' ? 'active' : ''}" onclick="switchChat('🤖 AI 管家')" id="contact-🤖 AI 管家">
                    <img src="\${getAvatarUrl('🤖 AI 管家')}" class="avatar-img" style="width:30px; height:30px;" />
                    <span>🤖 AI 管家 <span id="badge-🤖 AI 管家" class="unread-badge friend-badge" data-friend="🤖 AI 管家" style="display:none"></span></span>
                </div>
            \`;
            friends.forEach(f => {
                if(!f || !f.account_id || f.account_id === '🤖 AI 管家') return; 
                let friendId = f.account_id; let displayName = f.alias ? f.alias : (f.nickname ? f.nickname : friendId);
                const div = document.createElement('div'); div.className = 'contact-item ' + (activeChat === friendId ? 'active' : ''); div.id = 'contact-' + friendId;
                div.innerHTML = \`
                    <img src="\${f.avatar_url || getAvatarUrl(friendId)}" class="avatar-img" style="width:30px; height:30px;" />
                    <span title="特工代号: \${friendId}">\${displayName} <span class="unread-badge friend-badge" data-friend="\${friendId}" style="display:none"></span></span> 
                    <span class="sidebar-del-btn" onclick="removeFriend(event, '\${friendId}')">双向删除</span>
                \`; 
                div.onclick = () => switchChat(friendId); 
                panel.appendChild(div);
            });
            updateBadges();
        });

        function removeFriend(e, f) { 
            e.stopPropagation(); 
            if(confirm("【警告】确定删除好友 " + f + " 吗？\\n此操作将同时彻底销毁你们双方的聊天记录！")) { 
                socket.emit('remove friend', { me: myName, target: f }); 
            } 
        }
        socket.on('remove_res', () => { socket.emit('get friends', { username: myName }); });

        // 双向删除通知接管
        socket.on('force_clear_chat', (data) => {
            allMessages = allMessages.filter(m => {
                return !((m.receiver === data.target && m.user_id === myName) || (m.receiver === myName && m.user_id === data.target));
            });
            if (activeChat === data.target) switchChat('ALL');
            renderMessages();
        });

        // 🌟 响应 AI 管家清屏指令 (清理指定目标或大厅记录)
        socket.on('local_clear_chat_client', (data) => {
            allMessages = allMessages.filter(m => {
                if (data.target === 'ALL') return m.receiver !== 'ALL';
                return !((m.receiver === data.target && m.user_id === myName) || (m.receiver === myName && m.user_id === data.target));
            });
            if (activeChat === data.target || (data.target === 'ALL' && activeChat === 'ALL')) renderMessages();
        });

        function clearActiveChat() {
            if(confirm("【本地清除】确定要清空与 " + activeChat + " 的记录吗？\\n(这仅删除您自己的屏幕显示，对方的记录不会受影响)")) {
                socket.emit('clear chat', { username: myName, target: activeChat });
                allMessages = allMessages.filter(m => {
                    if (activeChat === 'ALL') return m.receiver !== 'ALL';
                    return !((m.receiver === activeChat && m.user_id === myName) || (m.receiver === myName && m.user_id === activeChat));
                });
                renderMessages();
            }
        }

        const fileInput = document.getElementById('fileInput');
        fileInput.addEventListener('change', async function() {
            const file = this.files[0]; if (!file) return;
            const tempId = uuidv4();
            allMessages.push({ msg_uuid: tempId, user_id: myName, receiver: activeChat, text: "[系统] 正在上传文件...", timestamp: getLocalTimeString(), type: 'text' });
            renderMessages();

            const formData = new FormData(); formData.append('mediaFile', file);
            try {
                const response = await fetch('/api/upload', { method: 'POST', body: formData });
                const result = await response.json();
                if (result.url) {
                    let mediaTag = '[FILE]';
                    if(result.type.startsWith('image/')) mediaTag = '[IMAGE]';
                    else if(result.type.startsWith('video/')) mediaTag = '[VIDEO]';
                    else if(result.type.startsWith('audio/')) mediaTag = '[AUDIO]';

                    const realMsgId = uuidv4();
                    const newMsg = { msg_uuid: realMsgId, user_id: myName, receiver: activeChat, text: mediaTag + result.url, is_recalled: false, timestamp: getLocalTimeString(), type: 'text' };
                    socket.emit('chat message', newMsg);
                    
                    allMessages = allMessages.filter(m => m.msg_uuid !== tempId);
                    allMessages.push(newMsg); renderMessages();
                }
            } catch (err) {
                allMessages = allMessages.filter(m => m.msg_uuid !== tempId); alert("文件上传失败！"); renderMessages();
            }
            this.value = ''; 
        });

        document.getElementById('chat-input').onkeypress = (e) => { if(e.key === 'Enter') sendMsg(); };

        function sendMsg() {
            const input = document.getElementById('chat-input'); const text = input.value.trim();
            if(text) {
                const msgId = uuidv4();
                const newMsg = { msg_uuid: msgId, user_id: myName, receiver: activeChat, text: text, is_recalled: false, timestamp: getLocalTimeString(), type: 'text' };
                socket.emit('chat message', newMsg);
                allMessages.push(newMsg); 
                renderMessages();
                input.value = '';
            }
        }

        socket.on('sync_res', (msgs) => { allMessages = msgs || []; renderMessages(); });

        socket.on('chat message', (msg) => {
            if (msg.type === 'system_command') { return; }
            if (msg.type === 'stream_chunk') {
                let textChunk = msg.text || '';
                let existingMsg = allMessages.find(m => m.msg_uuid === msg.msg_uuid);
                if (existingMsg) existingMsg.text += textChunk; 
                else { msg.text = textChunk; allMessages.push(msg); let source = msg.receiver === 'ALL' ? 'ALL' : msg.user_id; if (source !== myName && source !== activeChat) { unreadCounts[source] = (unreadCounts[source] || 0) + 1; updateBadges(); } }
                renderMessages(); return;
            }
            if (msg.type === 'stream_end') {
                let existingMsg = allMessages.find(m => m.msg_uuid === msg.msg_uuid);
                if (existingMsg) { existingMsg.text = msg.text || ''; existingMsg.type = 'text'; existingMsg.timestamp = msg.timestamp; } 
                else { msg.type = 'text'; msg.text = msg.text || ''; allMessages.push(msg); }
                renderMessages(); return;
            }
            if (!allMessages.find(m => m.msg_uuid === msg.msg_uuid)) { 
                msg.text = msg.text || '';
                allMessages.push(msg); 
                let source = msg.receiver === 'ALL' ? 'ALL' : msg.user_id;
                if (source !== myName && source !== activeChat) { unreadCounts[source] = (unreadCounts[source] || 0) + 1; updateBadges(); }
                renderMessages(); 
            }
        });

        socket.on('recall event', (data) => { const msg = allMessages.find(m => m.msg_uuid === data.msg_uuid); if(msg) msg.is_recalled = true; renderMessages(); });
        function recallMsg(uuid) { socket.emit('recall message', { username: myName, msg_uuid: uuid }); }
        function deleteLocalMsg(uuid) { socket.emit('delete local message', { username: myName, msg_uuid: uuid }); allMessages = allMessages.filter(m => m.msg_uuid !== uuid); renderMessages(); }

        function renderMessages() {
            const ul = document.getElementById('messages'); ul.innerHTML = '';
            
            const filteredMsgs = allMessages.filter(msg => {
                if (activeChat === 'ALL') return msg.receiver === 'ALL';
                return (msg.receiver === activeChat && msg.user_id === myName) || (msg.receiver === myName && msg.user_id === activeChat);
            });

            let lastTime = 0;

            filteredMsgs.forEach(msg => {
                let msgTime = 0;
                if (msg.timestamp) { let d = new Date(msg.timestamp.replace(' ', 'T')); if (!isNaN(d)) msgTime = d.getTime(); }
                
                if (msgTime > 0 && (msgTime - lastTime > 300000)) {
                    const divider = document.createElement('div'); divider.className = 'time-divider';
                    let d = new Date(msgTime); const now = new Date();
                    const isSameDay = d.getDate() === now.getDate() && d.getMonth() === now.getMonth() && d.getFullYear() === now.getFullYear();
                    const pad = (n) => n.toString().padStart(2, '0');
                    let hh = pad(d.getHours()); let mm = pad(d.getMinutes());
                    if (isSameDay) { divider.innerText = hh + ":" + mm; } else { divider.innerText = pad(d.getMonth() + 1) + "月" + pad(d.getDate()) + "日 " + hh + ":" + mm; }
                    ul.appendChild(divider);
                }
                if(msgTime > 0) lastTime = msgTime;

                const li = document.createElement('li'); li.id = 'msg-' + msg.msg_uuid; li.className = "msg-row " + (msg.user_id === myName ? "me" : "other");
                
                if (msg.is_recalled) { 
                    li.innerHTML = "<div class='msg-system'>此消息已被撤回</div>"; 
                } 
                else {
                    let senderName = msg.user_id === myName ? '我' : msg.user_id; let actions = "";
                    if(msg.user_id === myName && msg.type !== 'stream_chunk') actions += \`<span class="action-link" onclick="recallMsg('\${msg.msg_uuid}')">撤回</span> | \`;
                    if(msg.type !== 'stream_chunk') actions += \`<span class="action-link" onclick="deleteLocalMsg('\${msg.msg_uuid}')">删除</span>\`;
                    
                    let displayContent = msg.text || "";
                    if (displayContent.startsWith('[IMAGE]')) {
                        let url = displayContent.replace('[IMAGE]', ''); displayContent = \`<img src="\${url}" style="max-width:220px; border-radius:8px; cursor:pointer;" onclick="window.open('\${url}')" />\`;
                    } else if (displayContent.startsWith('[VIDEO]')) {
                        let url = displayContent.replace('[VIDEO]', ''); displayContent = \`<video src="\${url}" controls style="max-width:250px; border-radius:8px;"></video>\`;
                    } else if (displayContent.startsWith('[AUDIO]')) {
                        let url = displayContent.replace('[AUDIO]', ''); displayContent = \`<audio src="\${url}" controls style="width:250px;"></audio>\`;
                    }

                    if (msg.type === 'stream_chunk') { displayContent += '<span class="streaming-cursor"></span>'; }

                    let avatarUrl = getAvatarUrl(msg.user_id);
                    if(msg.user_id === myName) { const myAvatarImg = document.getElementById('my-avatar'); if(myAvatarImg && myAvatarImg.src) avatarUrl = myAvatarImg.src; }
                    
                    li.innerHTML = \`
                        <div class="msg-content-wrapper">
                            <img src="\${avatarUrl}" class="avatar-img" />
                            <div style="display:flex; flex-direction:column; \${msg.user_id === myName ? 'align-items:flex-end' : 'align-items:flex-start'}">
                                <div style="font-size:12px; color:#64748b; margin-bottom:6px; margin-left:4px; margin-right:4px;">\${senderName}</div>
                                <div class="msg-bubble">\${displayContent}</div>
                                <div class="msg-actions">\${actions}</div>
                            </div>
                        </div>
                    \`;
                }
                ul.appendChild(li);
            });
            ul.scrollTop = ul.scrollHeight;
        }
    </script>
    </body>
    </html>
  `);
});

io.on('connection', (socket) => {
  let userGrpcStream = null; let currentUser = null;

  socket.on('register', (data) => { grpcClient.Register({ account_id: data.username, nickname: data.username, password: data.password }, (err, res) => { socket.emit('register_res', err ? { success: false, message: "核心离线" } : res); }); });
  
  socket.on('login', (data) => {
      grpcClient.Login({ account_id: data.username, password: data.password }, (err, res) => {
          if (err) return socket.emit('login_res', { success: false, message: "核心离线" });
          socket.emit('login_res', res);
          if (res.success) {
              currentUser = data.username;
              userGrpcStream = grpcClient.ChatStream();
              userGrpcStream.write({ msg_uuid: "init-" + Math.random(), user_id: currentUser, receiver: "SYSTEM", text: "init", type: "system", timestamp: "now" });
              
              userGrpcStream.on('data', (msg) => {
                  if (msg.type === 'recall_event') {
                      socket.emit('recall event', { msg_uuid: msg.msg_uuid });
                  } else if (msg.type === 'system_command') {
                      if (msg.text === 'NEW_FRIEND_REQUEST') {
                          grpcClient.GetPendingRequests({ account_id: currentUser }, (err, res) => { if (res) socket.emit('pending_res', res.pending_users || []); });
                      } else if (msg.text === 'REFRESH_FRIENDS') {
                          grpcClient.GetFriendsList({ account_id: currentUser }, (err, res) => { if (res) socket.emit('friends_res', res.friends || []); });
                      } else if (msg.text.startsWith('FRIEND_DELETED:')) {
                          const deletedUser = msg.text.split(':')[1];
                          grpcClient.GetFriendsList({ account_id: currentUser }, (err, res) => { if (res) socket.emit('friends_res', res.friends || []); });
                          socket.emit('force_clear_chat', { target: deletedUser });
                      } else if (msg.text === 'FORCE_LOGOUT') { // 🌟 捕获后端注销指令
                          socket.emit('force_logout_client');
                      } else if (msg.text.startsWith('LOCAL_CLEAR_CHAT:')) { // 🌟 捕获后端清屏指令
                          const target = msg.text.split(':')[1];
                          socket.emit('local_clear_chat_client', { target: target });
                      }
                  } else { socket.emit('chat message', msg); }
              });
              userGrpcStream.on('error', () => {});
          }
      });
  });

  socket.on('send request', (data) => { grpcClient.SendFriendRequest({ my_account: data.me, target_account: data.target }, (err, res) => { socket.emit('req_res', res); }); });
  socket.on('get pending', (data) => { grpcClient.GetPendingRequests({ account_id: data.username }, (err, res) => { socket.emit('pending_res', res ? res.pending_users : []); }); });
  socket.on('handle request', (data) => { 
      grpcClient.HandleFriendRequest({ my_account: data.me, sender_account: data.sender, accept: data.accept }, (err, res) => { 
          socket.emit('handle_res', res); 
          if (data.accept && userGrpcStream) { userGrpcStream.write({ msg_uuid: "sys-" + Math.random(), user_id: currentUser, receiver: data.sender, text: "REFRESH_FRIENDS", type: "system_command", timestamp: "now" }); }
      }); 
  });
  
  socket.on('remove friend', (data) => { grpcClient.RemoveFriend({ my_account: data.me, target_account: data.target }, (err, res) => { socket.emit('remove_res', res); }); });
  socket.on('get friends', (data) => { grpcClient.GetFriendsList({ account_id: data.username }, (err, res) => { socket.emit('friends_res', res ? res.friends : []); }); });
  
  socket.on('recall message', (data) => { grpcClient.RecallMessage({ account_id: data.username, msg_uuid: data.msg_uuid }, (err, res) => { if (res && !res.success) socket.emit('recall_error', res.message); }); });
  socket.on('delete local message', (data) => { grpcClient.DeleteLocalMessage({ account_id: data.username, msg_uuid: data.msg_uuid }, () => {}); });
  socket.on('clear chat', (data) => { grpcClient.ClearChat({ account_id: data.username, target_account: data.target }, () => {}); });
  socket.on('sync messages', (data) => { grpcClient.SyncMessages({ account_id: data.username }, (err, res) => { socket.emit('sync_res', res ? res.messages : []); }); });
  
  socket.on('chat message', (msg) => { 
      if (userGrpcStream && currentUser) { 
          let timeToSave = msg.timestamp || new Date().toISOString().replace('T', ' ').substring(0, 19);
          userGrpcStream.write({ msg_uuid: msg.msg_uuid, user_id: currentUser, receiver: msg.receiver, text: msg.text || "", timestamp: timeToSave }); 
      } 
  });
  socket.on('disconnect', () => { if (userGrpcStream) userGrpcStream.end(); });
});

http.listen(3000, '0.0.0.0', () => { 
    console.log('✨ CCHAT 高级流式网关 (AI接管版) 已启动...'); 
});