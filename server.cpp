#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <algorithm>
#include <thread>
#include <chrono>
#include <functional> 
#include <sstream>
#include <cmath> 
#include <future> 
#include <grpcpp/grpcpp.h>
#include <hiredis/hiredis.h>
#include <mysql/mysql.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "chat.grpc.pb.h"

using namespace grpc;
using namespace chat;
using json = nlohmann::json;

std::map<std::string, ServerReaderWriter<ChatMessage, ChatMessage>*> global_clients;
std::mutex global_mtx;

void AutoPatchDatabase() {
    MYSQL* conn = mysql_init(NULL);
    if (mysql_real_connect(conn, "127.0.0.1", "cchat_user", "123456", "cchat_db", 3306, NULL, 0)) {
        if (mysql_query(conn, "ALTER TABLE chat_history ADD COLUMN embedding JSON") == 0) {
            std::cout << "✅ 自动补丁：成功为 chat_history 植入 AI 记忆字段！" << std::endl;
        }
        mysql_close(conn);
    }
}

size_t StringWriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

struct CurlStreamContext {
    std::string full_response;
    std::function<void(const std::string&)> on_chunk;
};

size_t StreamWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    std::string chunk((char*)contents, realsize);
    CurlStreamContext* ctx = static_cast<CurlStreamContext*>(userp);

    std::string::size_type pos = 0;
    while ((pos = chunk.find("data: ", pos)) != std::string::npos) {
        pos += 6;
        std::string::size_type end_pos = chunk.find('\n', pos);
        if (end_pos == std::string::npos) break;
        std::string json_str = chunk.substr(pos, end_pos - pos);
        if (json_str.find("[DONE]") != std::string::npos) break;
        try {
            json j = json::parse(json_str);
            if (j.contains("choices") && j["choices"].size() > 0) {
                auto choice = j["choices"][0];
                if (choice.contains("delta") && choice["delta"].contains("content")) {
                    std::string content = choice["delta"]["content"];
                    ctx->full_response += content;
                    if (ctx->on_chunk) ctx->on_chunk(content);
                }
            }
        } catch(...) {}
    }
    return realsize;
}
//流式回调 AI 每吐出一个字，它就拦截下来，通过 Redis 广播出去

bool IsValidForMemory(const std::string& text) {
    if (text.empty() || text.length() <= 3) return false;
    int junk_count = 0;
    for (char c : text) { if (std::isdigit(c) || std::isspace(c) || c == '.' || c == '?' || c == '!') junk_count++; }
    if (junk_count == text.length()) return false; 
    return true;
}

std::vector<double> GetEmbedding(const std::string& text) {
    std::vector<double> embedding;
    if (text.empty() || !IsValidForMemory(text)) return embedding;

    CURL* curl = curl_easy_init();
    if(curl) {
        std::string api_key = "sk-177ac80c0a2848598be7d646b2ea871f"; 
        curl_easy_setopt(curl, CURLOPT_URL, "https://dashscope.aliyuncs.com/api/v1/services/embeddings/text-embedding/text-embedding");
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        std::string payload_str;
        try {
            json payload = { {"model", "text-embedding-v2"}, {"input", { {"texts", {text}} }} };
            payload_str = payload.dump();
        } catch (...) { return embedding; }

        std::string readBuffer;
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StringWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        
        if(curl_easy_perform(curl) == CURLE_OK) {
            try {
                json response = json::parse(readBuffer);
                if (response.contains("output") && response["output"].contains("embeddings")) {
                    auto emb_array = response["output"]["embeddings"][0]["embedding"];
                    for (auto& val : emb_array) { embedding.push_back(val.get<double>()); }
                }
            } catch (...) { }
        }
        curl_slist_free_all(headers); curl_easy_cleanup(curl);
    }
    return embedding;
}

double CosineSimilarity(const std::vector<double>& A, const std::vector<double>& B) {
    if (A.size() != B.size() || A.empty()) return 0.0;
    double dot_product = 0.0, norm_A = 0.0, norm_B = 0.0;
    for (size_t i = 0; i < A.size(); ++i) { dot_product += A[i] * B[i]; norm_A += A[i] * A[i]; norm_B += B[i] * B[i]; }
    if (norm_A == 0.0 || norm_B == 0.0) return 0.0;
    return dot_product / (std::sqrt(norm_A) * std::sqrt(norm_B));
}

std::string AskAI(const std::string& dynamic_system_prompt, const std::string& question, const std::vector<json>& history, std::function<void(const std::string&)> on_chunk = nullptr) {
    CURL* curl = curl_easy_init();
    CurlStreamContext ctx; ctx.on_chunk = on_chunk;

    if(curl) {
        std::string api_key = "sk-177ac80c0a2848598be7d646b2ea871f"; 
        curl_easy_setopt(curl, CURLOPT_URL, "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
        
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, ("Authorization: Bearer " + api_key).c_str());
        headers = curl_slist_append(headers, "Accept: text/event-stream");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

        std::string payload_str;
        try {
            json messages = json::array();
            messages.push_back({{"role", "system"}, {"content", dynamic_system_prompt}});
            for (const auto& h : history) { messages.push_back(h); }
            messages.push_back({{"role", "user"}, {"content", question}});
            json payload = { {"model", "qwen-plus"}, {"messages", messages}, {"temperature", 0.3}, {"stream", on_chunk != nullptr} };
            payload_str = payload.dump();
        } catch (...) { return "⚠️ 内部序列化异常"; }

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload_str.c_str());
        
        if (on_chunk != nullptr) {
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        } else {
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StringWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &(ctx.full_response));
        }
        
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            ctx.full_response = "⚠️ 网络请求超时。";
        } else if (on_chunk == nullptr) {
            try {
                json response = json::parse(ctx.full_response);
                if (response.contains("choices") && response["choices"].size() > 0) {
                    ctx.full_response = response["choices"][0]["message"]["content"];
                }
            } catch (...) { }
        }
        curl_slist_free_all(headers); curl_easy_cleanup(curl);
    }
    return ctx.full_response;
}

void RedisSubscriber() {
    redisContext *c = redisConnect("127.0.0.1", 6379);
    if (c == NULL || c->err) return;
    redisReply *reply = (redisReply *)redisCommand(c, "SUBSCRIBE cchat_channel"); freeReplyObject(reply);
    while(redisGetReply(c, (void **)&reply) == REDIS_OK) {
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) {
            if (strcmp(reply->element[0]->str, "message") == 0) {
                std::string payload(reply->element[2]->str, reply->element[2]->len); ChatMessage msg;
                if (msg.ParseFromString(payload)) {
                    std::lock_guard<std::mutex> lock(global_mtx);
                    if (msg.receiver() == "ALL") { for (auto const& [user, stream] : global_clients) stream->Write(msg); } 
                    else {
                        if (global_clients.count(msg.receiver())) global_clients[msg.receiver()]->Write(msg);
                        if (global_clients.count(msg.user_id()) && msg.user_id() != msg.receiver()) global_clients[msg.user_id()]->Write(msg);
                    }
                }
            }
        }
        freeReplyObject(reply);
    }
}

void SaveMessageToMySQL(const ChatMessage& msg) {
    if (msg.type() == "recall_event" || msg.type() == "system" || msg.type() == "system_command" || msg.type() == "stream_chunk") return; 
    std::thread([msg]() {
        std::vector<double> emb = GetEmbedding(msg.text());
        std::string emb_sql_val = "NULL";
        
        if (!emb.empty()) { 
            json j = emb; emb_sql_val = "'" + j.dump() + "'"; 
        }

        MYSQL* conn = mysql_init(NULL);
        if (mysql_real_connect(conn, "127.0.0.1", "cchat_user", "123456", "cchat_db", 3306, NULL, 0)) {
            mysql_set_character_set(conn, "utf8mb4"); 
            std::vector<char> safe_uuid(msg.msg_uuid().length() * 2 + 1); std::vector<char> safe_user(msg.user_id().length() * 2 + 1);
            std::vector<char> safe_recv(msg.receiver().length() * 2 + 1); std::vector<char> safe_text(msg.text().length() * 2 + 1);
            mysql_real_escape_string(conn, safe_uuid.data(), msg.msg_uuid().c_str(), msg.msg_uuid().length());
            mysql_real_escape_string(conn, safe_user.data(), msg.user_id().c_str(), msg.user_id().length());
            mysql_real_escape_string(conn, safe_recv.data(), msg.receiver().c_str(), msg.receiver().length());
            mysql_real_escape_string(conn, safe_text.data(), msg.text().c_str(), msg.text().length());
            
            std::string q = "INSERT INTO chat_history (msg_uuid, username, receiver, message_text, embedding) VALUES ('" 
                            + std::string(safe_uuid.data()) + "', '" + std::string(safe_user.data()) + "', '" 
                            + std::string(safe_recv.data()) + "', '" + std::string(safe_text.data()) + "', " + emb_sql_val + ")";
            if (mysql_query(conn, q.c_str()) != 0) {
                std::string fallback_q = "INSERT INTO chat_history (msg_uuid, username, receiver, message_text) VALUES ('" 
                            + std::string(safe_uuid.data()) + "', '" + std::string(safe_user.data()) + "', '" 
                            + std::string(safe_recv.data()) + "', '" + std::string(safe_text.data()) + "')";
                mysql_query(conn, fallback_q.c_str());
            }
        }
        mysql_close(conn);
    }).detach();
}

class ChatServiceImpl final : public ChatService::Service {
public:
    void PublishToRedis(const ChatMessage& msg) {
        redisContext *c = redisConnect("127.0.0.1", 6379);
        if (c != NULL && !c->err) {
            std::string payload; msg.SerializeToString(&payload); 
            redisReply *reply = (redisReply *)redisCommand(c, "PUBLISH cchat_channel %b", payload.data(), payload.size());
            if (reply) freeReplyObject(reply); redisFree(c);
        } else if (c) redisFree(c);
    }
    
    MYSQL* GetDBConn() { 
        MYSQL* conn = mysql_init(NULL); 
        if (mysql_real_connect(conn, "127.0.0.1", "cchat_user", "123456", "cchat_db", 3306, NULL, 0)) { mysql_set_character_set(conn, "utf8mb4"); return conn; }
        return NULL; 
    }
    
    std::string GenerateSysId() { return "sys-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()); }

    Status Register(ServerContext* context, const RegisterRequest* request, StandardResponse* reply) override { 
        MYSQL* conn = GetDBConn(); if(!conn) { reply->set_success(false); return Status::OK; }
        std::vector<char> acc(request->account_id().length() * 2 + 1); std::vector<char> nick(request->nickname().length() * 2 + 1); std::vector<char> pwd(request->password().length() * 2 + 1);
        mysql_real_escape_string(conn, acc.data(), request->account_id().c_str(), request->account_id().length());
        mysql_real_escape_string(conn, nick.data(), request->nickname().c_str(), request->nickname().length());
        mysql_real_escape_string(conn, pwd.data(), request->password().c_str(), request->password().length());
        std::string q = "INSERT INTO users (account_id, nickname, username, password) VALUES ('" + std::string(acc.data()) + "', '" + std::string(nick.data()) + "', '" + std::string(acc.data()) + "', SHA2('" + std::string(pwd.data()) + "', 256))";
        if(mysql_query(conn, q.c_str()) == 0) { reply->set_success(true); reply->set_message("注册成功"); } else { reply->set_success(false); reply->set_message("该账号已被注册"); }
        mysql_close(conn); return Status::OK;
    }

    Status Login(ServerContext* context, const LoginRequest* request, LoginResponse* reply) override { 
        MYSQL* conn = GetDBConn(); if(!conn) { reply->set_success(false); return Status::OK; }
        std::vector<char> acc(request->account_id().length() * 2 + 1); std::vector<char> pwd(request->password().length() * 2 + 1);
        mysql_real_escape_string(conn, acc.data(), request->account_id().c_str(), request->account_id().length()); 
        mysql_real_escape_string(conn, pwd.data(), request->password().c_str(), request->password().length());
        std::string q = "SELECT account_id, nickname, avatar_url FROM users WHERE account_id = '" + std::string(acc.data()) + "' AND password = SHA2('" + std::string(pwd.data()) + "', 256)";
        if(mysql_query(conn, q.c_str()) == 0) { 
            MYSQL_RES* res = mysql_store_result(conn); 
            if(res && mysql_num_rows(res) > 0) { 
                MYSQL_ROW row = mysql_fetch_row(res);
                reply->set_success(true); reply->set_message("登录成功");
                reply->set_account_id(row[0] ? row[0] : ""); reply->set_nickname(row[1] ? row[1] : ""); reply->set_avatar_url(row[2] ? row[2] : "");
            } else { reply->set_success(false); reply->set_message("账号或密码错误"); } 
            if(res) mysql_free_result(res); 
        }
        mysql_close(conn); return Status::OK;
    }

    Status UpdateProfile(ServerContext* context, const UpdateProfileRequest* request, StandardResponse* reply) override { return Status::OK; } 
    Status SetFriendAlias(ServerContext* context, const SetAliasRequest* request, StandardResponse* reply) override { return Status::OK; } 

    Status SendFriendRequest(ServerContext* context, const FriendReqMsg* request, StandardResponse* reply) override {
        MYSQL* conn = GetDBConn(); std::vector<char> me(request->my_account().length() * 2 + 1); std::vector<char> target(request->target_account().length() * 2 + 1);
        mysql_real_escape_string(conn, me.data(), request->my_account().c_str(), request->my_account().length()); mysql_real_escape_string(conn, target.data(), request->target_account().c_str(), request->target_account().length());
        std::string check_q = "SELECT account_id FROM users WHERE account_id='" + std::string(target.data()) + "'";
        if(mysql_query(conn, check_q.c_str()) == 0) {
            MYSQL_RES* res = mysql_store_result(conn);
            if(res && mysql_num_rows(res) > 0) {
                std::string insert_q = "INSERT IGNORE INTO friend_requests (sender, receiver) VALUES ('" + std::string(me.data()) + "', '" + std::string(target.data()) + "')";
                mysql_query(conn, insert_q.c_str());
                if (mysql_affected_rows(conn) > 0) { ChatMessage sys_msg; sys_msg.set_msg_uuid(GenerateSysId()); sys_msg.set_user_id("SYSTEM"); sys_msg.set_receiver(std::string(target.data())); sys_msg.set_type("system_command"); sys_msg.set_text("NEW_FRIEND_REQUEST"); PublishToRedis(sys_msg); }
                reply->set_success(true); reply->set_message("好友申请已发送！");
            } else { reply->set_success(false); reply->set_message("发送失败：该用户不存在！"); }
            if(res) mysql_free_result(res);
        }
        mysql_close(conn); return Status::OK;
    }

    Status GetPendingRequests(ServerContext* context, const UserRequest* request, PendingListResponse* reply) override {
        MYSQL* conn = GetDBConn(); std::vector<char> me(request->account_id().length() * 2 + 1); mysql_real_escape_string(conn, me.data(), request->account_id().c_str(), request->account_id().length());
        std::string q = "SELECT r.sender, u.nickname, u.avatar_url FROM friend_requests r JOIN users u ON r.sender = u.account_id WHERE r.receiver='" + std::string(me.data()) + "' AND r.status='pending'";
        if(mysql_query(conn, q.c_str()) == 0) { MYSQL_RES* res = mysql_store_result(conn); MYSQL_ROW row; while((row = mysql_fetch_row(res))) { 
            FriendInfo* f = reply->add_pending_users(); f->set_account_id(row[0] ? row[0] : ""); f->set_nickname(row[1] ? row[1] : ""); f->set_avatar_url(row[2] ? row[2] : "");
        } mysql_free_result(res); }
        mysql_close(conn); return Status::OK;
    }

    Status HandleFriendRequest(ServerContext* context, const HandleReqMsg* request, StandardResponse* reply) override {
        MYSQL* conn = GetDBConn(); std::vector<char> me(request->my_account().length() * 2 + 1); std::vector<char> sender(request->sender_account().length() * 2 + 1);
        mysql_real_escape_string(conn, me.data(), request->my_account().c_str(), request->my_account().length()); mysql_real_escape_string(conn, sender.data(), request->sender_account().c_str(), request->sender_account().length());
        if (request->accept()) { 
            std::string q1 = "INSERT IGNORE INTO friends (user_a, user_b) VALUES ('" + std::string(sender.data()) + "', '" + std::string(me.data()) + "')"; 
            std::string q2 = "INSERT IGNORE INTO friends (user_a, user_b) VALUES ('" + std::string(me.data()) + "', '" + std::string(sender.data()) + "')"; 
            mysql_query(conn, q1.c_str()); mysql_query(conn, q2.c_str());
            ChatMessage sys_msg; sys_msg.set_msg_uuid(GenerateSysId()); sys_msg.set_user_id("SYSTEM"); sys_msg.set_receiver(std::string(sender.data())); sys_msg.set_type("system_command"); sys_msg.set_text("REFRESH_FRIENDS"); PublishToRedis(sys_msg);
        }
        std::string q3 = "DELETE FROM friend_requests WHERE sender='" + std::string(sender.data()) + "' AND receiver='" + std::string(me.data()) + "'"; mysql_query(conn, q3.c_str());
        reply->set_success(true); reply->set_message("处理成功"); mysql_close(conn); return Status::OK;
    }

    Status RemoveFriend(ServerContext* context, const FriendReqMsg* request, StandardResponse* reply) override {
        MYSQL* conn = GetDBConn(); std::vector<char> me(request->my_account().length() * 2 + 1); std::vector<char> target(request->target_account().length() * 2 + 1);
        mysql_real_escape_string(conn, me.data(), request->my_account().c_str(), request->my_account().length()); mysql_real_escape_string(conn, target.data(), request->target_account().c_str(), request->target_account().length());
        
        std::string q = "DELETE FROM friends WHERE (user_a='" + std::string(me.data()) + "' AND user_b='" + std::string(target.data()) + "') OR (user_a='" + std::string(target.data()) + "' AND user_b='" + std::string(me.data()) + "')"; 
        mysql_query(conn, q.c_str());

        std::string q_del_msg = "DELETE FROM chat_history WHERE (username='" + std::string(me.data()) + "' AND receiver='" + std::string(target.data()) + "') OR (username='" + std::string(target.data()) + "' AND receiver='" + std::string(me.data()) + "')";
        mysql_query(conn, q_del_msg.c_str());

        ChatMessage sys_msg; sys_msg.set_msg_uuid(GenerateSysId()); sys_msg.set_user_id("SYSTEM"); sys_msg.set_receiver(std::string(target.data())); sys_msg.set_type("system_command"); sys_msg.set_text("FRIEND_DELETED:" + std::string(me.data())); PublishToRedis(sys_msg);
        ChatMessage sys_msg_me; sys_msg_me.set_msg_uuid(GenerateSysId()); sys_msg_me.set_user_id("SYSTEM"); sys_msg_me.set_receiver(std::string(me.data())); sys_msg_me.set_type("system_command"); sys_msg_me.set_text("FRIEND_DELETED:" + std::string(target.data())); PublishToRedis(sys_msg_me);

        reply->set_success(true); reply->set_message("双向物理销毁成功"); mysql_close(conn); return Status::OK;
    }

    Status GetFriendsList(ServerContext* context, const UserRequest* request, FriendsListResponse* reply) override {
        MYSQL* conn = GetDBConn(); std::vector<char> me(request->account_id().length() * 2 + 1); mysql_real_escape_string(conn, me.data(), request->account_id().c_str(), request->account_id().length());
        std::string q = "SELECT f.user_b, u.nickname, u.avatar_url, f.alias FROM friends f JOIN users u ON f.user_b = u.account_id WHERE f.user_a='" + std::string(me.data()) + "'";
        if(mysql_query(conn, q.c_str()) == 0) { 
            MYSQL_RES* res = mysql_store_result(conn); MYSQL_ROW row; 
            while((row = mysql_fetch_row(res))) { 
                FriendInfo* f = reply->add_friends();
                f->set_account_id(row[0] ? row[0] : ""); f->set_nickname(row[1] ? row[1] : ""); f->set_avatar_url(row[2] ? row[2] : ""); f->set_alias(row[3] ? row[3] : "");
            } mysql_free_result(res); 
        }
        mysql_close(conn); return Status::OK;
    }

    Status RecallMessage(ServerContext* context, const MessageActionRequest* request, StandardResponse* reply) override {
        MYSQL* conn = GetDBConn(); std::vector<char> uuid(request->msg_uuid().length() * 2 + 1); std::vector<char> usr(request->account_id().length() * 2 + 1);
        mysql_real_escape_string(conn, uuid.data(), request->msg_uuid().c_str(), request->msg_uuid().length()); mysql_real_escape_string(conn, usr.data(), request->account_id().c_str(), request->account_id().length());
        std::string q = "UPDATE chat_history SET is_recalled = 1 WHERE msg_uuid='" + std::string(uuid.data()) + "' AND username='" + std::string(usr.data()) + "' AND created_at >= NOW() - INTERVAL 2 MINUTE"; mysql_query(conn, q.c_str());
        if (mysql_affected_rows(conn) > 0) { ChatMessage recall_event; recall_event.set_msg_uuid(request->msg_uuid()); recall_event.set_type("recall_event"); recall_event.set_receiver("ALL"); PublishToRedis(recall_event); reply->set_success(true); } 
        else { reply->set_success(false); reply->set_message("撤回失败：消息不存在或已超 2 分钟！"); } mysql_close(conn); return Status::OK;
    }

    Status DeleteLocalMessage(ServerContext* context, const MessageActionRequest* request, StandardResponse* reply) override {
        MYSQL* conn = GetDBConn(); std::vector<char> uuid(request->msg_uuid().length() * 2 + 1); std::vector<char> usr(request->account_id().length() * 2 + 1);
        mysql_real_escape_string(conn, uuid.data(), request->msg_uuid().c_str(), request->msg_uuid().length()); mysql_real_escape_string(conn, usr.data(), request->account_id().c_str(), request->account_id().length());
        std::string q = "INSERT IGNORE INTO local_deleted_messages (username, msg_uuid) VALUES ('" + std::string(usr.data()) + "', '" + std::string(uuid.data()) + "')"; mysql_query(conn, q.c_str()); reply->set_success(true); mysql_close(conn); return Status::OK;
    }

    Status ClearChat(ServerContext* context, const ClearChatRequest* request, StandardResponse* reply) override {
        MYSQL* conn = GetDBConn(); std::vector<char> usr(request->account_id().length() * 2 + 1); std::vector<char> target(request->target_account().length() * 2 + 1);
        mysql_real_escape_string(conn, usr.data(), request->account_id().c_str(), request->account_id().length()); mysql_real_escape_string(conn, target.data(), request->target_account().c_str(), request->target_account().length()); std::string q;
        if (std::string(target.data()) == "ALL") q = "INSERT IGNORE INTO local_deleted_messages (username, msg_uuid) SELECT '" + std::string(usr.data()) + "', msg_uuid FROM chat_history WHERE receiver='ALL'";
        else q = "INSERT IGNORE INTO local_deleted_messages (username, msg_uuid) SELECT '" + std::string(usr.data()) + "', msg_uuid FROM chat_history WHERE (receiver='" + std::string(target.data()) + "' AND username='" + std::string(usr.data()) + "') OR (receiver='" + std::string(usr.data()) + "' AND username='" + std::string(target.data()) + "')";
        mysql_query(conn, q.c_str()); reply->set_success(true); mysql_close(conn); return Status::OK;
    }

    Status SyncMessages(ServerContext* context, const UserRequest* request, SyncResponse* reply) override {
        MYSQL* conn = GetDBConn(); std::vector<char> usr(request->account_id().length() * 2 + 1); mysql_real_escape_string(conn, usr.data(), request->account_id().c_str(), request->account_id().length());
        std::string q = "SELECT msg_uuid, username, receiver, message_text, created_at, is_recalled FROM chat_history WHERE created_at >= NOW() - INTERVAL 2 HOUR AND (receiver='ALL' OR receiver='" + std::string(usr.data()) + "' OR username='" + std::string(usr.data()) + "') AND msg_uuid NOT IN (SELECT msg_uuid FROM local_deleted_messages WHERE username='" + std::string(usr.data()) + "') ORDER BY created_at ASC";
        if(mysql_query(conn, q.c_str()) == 0) {
            MYSQL_RES* res = mysql_store_result(conn); MYSQL_ROW row;
            while((row = mysql_fetch_row(res))) {
                ChatMessage* msg = reply->add_messages();
                msg->set_msg_uuid(row[0] ? row[0] : ""); msg->set_user_id(row[1] ? row[1] : ""); msg->set_receiver(row[2] ? row[2] : "");
                msg->set_text(row[3] ? row[3] : ""); msg->set_timestamp(row[4] ? row[4] : ""); msg->set_is_recalled(row[5] && std::string(row[5]) == "1"); msg->set_type("text");
            } mysql_free_result(res);
        } mysql_close(conn); return Status::OK;
    }

    Status ChatStream(ServerContext* context, ServerReaderWriter<ChatMessage, ChatMessage>* stream) override {
        std::string current_user = ""; ChatMessage msg;
        while (stream->Read(&msg)) {
            if (current_user.empty()) {
                current_user = msg.user_id();
                std::lock_guard<std::mutex> lock(global_mtx); global_clients[current_user] = stream; 
            }
            if (msg.type() == "system" && msg.text() == "init") continue; 
            
            std::string text = msg.text();
            std::string active_chat = msg.receiver();
            std::string sender = msg.user_id();

            if (active_chat == "🤖 AI 管家" || text.find("@AI") == 0 || text.find("@ai") == 0) {
                msg.set_receiver("🤖 AI 管家"); 
                std::string question = text;
                if (text.find("@AI") == 0 || text.find("@ai") == 0) question = text.length() > 4 ? text.substr(4) : text;

                std::thread([this, question, sender, msg]() {
                    try {
                        SaveMessageToMySQL(msg); 
                        std::string stream_uuid = GenerateSysId();

                        auto future_short_term = std::async(std::launch::async, [this, sender]() {
                            std::vector<json> local_history;
                            MYSQL* conn = GetDBConn();
                            if (conn) {
                                std::vector<char> safe_sender(sender.length() * 2 + 1); 
                                mysql_real_escape_string(conn, safe_sender.data(), sender.c_str(), sender.length());
                                std::string q_recent = "SELECT username, message_text FROM chat_history WHERE is_recalled=0 AND msg_uuid NOT IN (SELECT msg_uuid FROM local_deleted_messages WHERE username='" + std::string(safe_sender.data()) + "') AND ((receiver='🤖 AI 管家' AND username='" + std::string(safe_sender.data()) + "') OR (receiver='" + std::string(safe_sender.data()) + "' AND username='🤖 AI 管家')) ORDER BY id DESC LIMIT 10";
                                if (mysql_query(conn, q_recent.c_str()) == 0) {
                                    MYSQL_RES* res = mysql_store_result(conn); MYSQL_ROW row;
                                    while ((row = mysql_fetch_row(res))) {
                                        std::string db_user = row[0] ? row[0] : ""; std::string db_text = row[1] ? row[1] : "";
                                        if (db_user == "🤖 AI 管家") local_history.push_back({{"role", "assistant"}, {"content", db_text}}); 
                                        else local_history.push_back({{"role", "user"}, {"content", db_text}});
                                    }
                                    mysql_free_result(res);
                                }
                                mysql_close(conn);
                            }
                            std::reverse(local_history.begin(), local_history.end());
                            return local_history;
                        });

                        auto future_long_term = std::async(std::launch::async, [this, sender, question]() {
                            std::string rag_str = "";
                            MYSQL* conn = GetDBConn();
                            if (conn) {
                                std::vector<char> safe_sender(sender.length() * 2 + 1); 
                                mysql_real_escape_string(conn, safe_sender.data(), sender.c_str(), sender.length());
                                
                                std::vector<double> query_emb = GetEmbedding(question);

                                if (!query_emb.empty()) {
                                    std::string q = "SELECT message_text, embedding FROM chat_history WHERE is_recalled=0 AND embedding IS NOT NULL AND username='" + std::string(safe_sender.data()) + "' AND receiver='🤖 AI 管家'";
                                    std::vector<std::pair<double, std::string>> scored_memories;

                                    if (mysql_query(conn, q.c_str()) == 0) {
                                        MYSQL_RES* res = mysql_store_result(conn); MYSQL_ROW row;
                                        while ((row = mysql_fetch_row(res))) {
                                            std::string db_text = row[0] ? row[0] : ""; std::string emb_str = row[1] ? row[1] : "";
                                            
                                            if (db_text.find("？") != std::string::npos || db_text.find("?") != std::string::npos || 
                                                db_text.find("吗") != std::string::npos || db_text.find("哪") != std::string::npos ||
                                                db_text.find("什么") != std::string::npos || db_text.find("怎么") != std::string::npos ||
                                                db_text.find("谁") != std::string::npos || db_text.find("帮我") != std::string::npos || 
                                                db_text.find("清除") != std::string::npos || db_text.find("删除") != std::string::npos) {
                                                continue;
                                            }

                                            try {
                                                json j_emb = json::parse(emb_str); std::vector<double> db_emb;
                                                for (auto& val : j_emb) db_emb.push_back(val.get<double>());
                                                double score = CosineSimilarity(query_emb, db_emb);
                                                if (score > 0.40 && db_text != question) scored_memories.push_back({score, db_text});
                                            } catch(...) {}
                                        }
                                        mysql_free_result(res);
                                    }
                                    std::sort(scored_memories.begin(), scored_memories.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                                    
                                    if (!scored_memories.empty()) {
                                        rag_str = "\n【提取到以下历史事实记忆】：\n";
                                        int limit = std::min(5, (int)scored_memories.size());
                                        for (int i = 0; i < limit; ++i) {
                                            rag_str += "- " + scored_memories[i].second + "\n";
                                        }
                                    }
                                }
                                mysql_close(conn);
                            }
                            return rag_str;
                        });

                        std::vector<json> history = future_short_term.get();
                        std::string rag_context = future_long_term.get();
                        
                       // 🌟 1. 极致精简且加入严苛防错规则的提示词
                        std::string dynamic_system_prompt = 
                            "你是 CCHAT 全能终端助理。请遵循指令：\n"
                            "1. 极致简短，严禁废话、解释和任何感情色彩。\n"
                            "2. 提取并直接输出下方记忆中的答案，若未知则回复“未知”。\n"
                            "【动作执行指令】(若用户要求执行操作，请在结尾附加纯JSON)：\n"
                            "换头像: {\"command\": \"UPDATE_AVATAR\", \"url\": \"...\"}\n"
                            "改备注: {\"command\": \"SET_ALIAS\", \"target\": \"账号\", \"alias\": \"备注名\"}\n"
                            "加好友: {\"command\": \"ADD_FRIEND\", \"target\": \"账号\"}\n"
                            "删除好友: {\"command\": \"REMOVE_FRIEND\", \"target\": \"账号\"}\n"
                            "清空大厅: {\"command\": \"CLEAR_CHAT\", \"target\": \"ALL\"}\n"
                            "清空私聊: {\"command\": \"CLEAR_CHAT\", \"target\": \"账号\"}\n"
                            "注销退出: {\"command\": \"LOGOUT\"}\n"
                            "发私聊: {\"command\": \"SEND_PRIVATE\", \"target\": \"账号\", \"content\": \"内容\"}\n"
                            "发大厅消息: {\"command\": \"SEND_PUBLIC\", \"content\": \"内容\"}\n"
                            "⚠️严重警告：target 字段必须是极其纯粹的账号名(例如'12','13')，绝对不能带有'好友'、'账号'等修饰词！\n" + rag_context;

                        bool is_first_chunk = true;
                        bool is_command_mode = false;
                        
                        std::string ai_reply = AskAI(dynamic_system_prompt, question, history, [&](const std::string& chunk_text) {
                            if (is_first_chunk) {
                                if (chunk_text.find("{") != std::string::npos) is_command_mode = true;
                                is_first_chunk = false;
                            }
                            if (!is_command_mode) {
                                ChatMessage chunk_msg; chunk_msg.set_msg_uuid(stream_uuid); chunk_msg.set_user_id("🤖 AI 管家"); chunk_msg.set_receiver(sender); chunk_msg.set_text(chunk_text); chunk_msg.set_type("stream_chunk"); this->PublishToRedis(chunk_msg);
                            }
                        });
                        
                        // 🌟 2. 增强版指令解析：补全实时通知与发信功能
                        if (ai_reply.find("\"command\"") != std::string::npos) {
                            try {
                                std::string clean_json = ai_reply;
                                size_t start = clean_json.find("{"); size_t end = clean_json.rfind("}");
                                if (start != std::string::npos && end != std::string::npos && end >= start) {
                                    clean_json = clean_json.substr(start, end - start + 1);
                                    json ai_cmd = json::parse(clean_json);
                                    if (ai_cmd.contains("command")) {
                                        std::string cmd = ai_cmd["command"];
                                        MYSQL* conn_cmd = GetDBConn();
                                        if(conn_cmd) {
                                            std::vector<char> safe_sender(sender.length() * 2 + 1); mysql_real_escape_string(conn_cmd, safe_sender.data(), sender.c_str(), sender.length());
                                            
                                            if (cmd == "UPDATE_AVATAR") {
                                                std::string url = ai_cmd["url"]; mysql_query(conn_cmd, ("UPDATE users SET avatar_url='" + url + "' WHERE account_id='" + std::string(safe_sender.data()) + "'").c_str());
                                            } else if (cmd == "SET_ALIAS") {
                                                std::string target = ai_cmd["target"]; std::string alias = ai_cmd["alias"];
                                                mysql_query(conn_cmd, ("UPDATE friends SET alias='" + alias + "' WHERE user_a='" + std::string(safe_sender.data()) + "' AND user_b='" + target + "'").c_str());
                                                ChatMessage sys_cmd; sys_cmd.set_msg_uuid(GenerateSysId()); sys_cmd.set_user_id("SYSTEM"); sys_cmd.set_receiver(sender); sys_cmd.set_text("REFRESH_FRIENDS"); sys_cmd.set_type("system_command"); this->PublishToRedis(sys_cmd);
                                            } else if (cmd == "ADD_FRIEND") {
                                                std::string target = ai_cmd["target"];
                                                mysql_query(conn_cmd, ("INSERT IGNORE INTO friend_requests (sender, receiver) VALUES ('" + std::string(safe_sender.data()) + "', '" + target + "')").c_str());
                                                // 修复：强制对方页面瞬间弹出好友申请！
                                                ChatMessage sys_cmd; sys_cmd.set_msg_uuid(GenerateSysId()); sys_cmd.set_user_id("SYSTEM"); sys_cmd.set_receiver(target); sys_cmd.set_text("NEW_FRIEND_REQUEST"); sys_cmd.set_type("system_command"); this->PublishToRedis(sys_cmd);
                                            } else if (cmd == "REMOVE_FRIEND") {
                                                std::string target = ai_cmd["target"];
                                                std::string q = "DELETE FROM friends WHERE (user_a='" + std::string(safe_sender.data()) + "' AND user_b='" + target + "') OR (user_a='" + target + "' AND user_b='" + std::string(safe_sender.data()) + "')"; 
                                                mysql_query(conn_cmd, q.c_str());
                                                std::string q_del_msg = "DELETE FROM chat_history WHERE (username='" + std::string(safe_sender.data()) + "' AND receiver='" + target + "') OR (username='" + target + "' AND receiver='" + std::string(safe_sender.data()) + "')";
                                                mysql_query(conn_cmd, q_del_msg.c_str());
                                                ChatMessage sys_msg; sys_msg.set_msg_uuid(GenerateSysId()); sys_msg.set_user_id("SYSTEM"); sys_msg.set_receiver(target); sys_msg.set_type("system_command"); sys_msg.set_text("FRIEND_DELETED:" + std::string(safe_sender.data())); this->PublishToRedis(sys_msg);
                                                ChatMessage sys_msg_me; sys_msg_me.set_msg_uuid(GenerateSysId()); sys_msg_me.set_user_id("SYSTEM"); sys_msg_me.set_receiver(sender); sys_msg_me.set_type("system_command"); sys_msg_me.set_text("FRIEND_DELETED:" + target); this->PublishToRedis(sys_msg_me);
                                            } else if (cmd == "CLEAR_CHAT") {
                                                std::string target = ai_cmd["target"]; std::string q;
                                                if (target == "ALL") { q = "INSERT IGNORE INTO local_deleted_messages (username, msg_uuid) SELECT '" + std::string(safe_sender.data()) + "', msg_uuid FROM chat_history WHERE receiver='ALL'"; } 
                                                else { q = "INSERT IGNORE INTO local_deleted_messages (username, msg_uuid) SELECT '" + std::string(safe_sender.data()) + "', msg_uuid FROM chat_history WHERE (receiver='" + target + "' AND username='" + std::string(safe_sender.data()) + "') OR (receiver='" + std::string(safe_sender.data()) + "' AND username='" + target + "')"; }
                                                mysql_query(conn_cmd, q.c_str());
                                                ChatMessage sys_cmd; sys_cmd.set_msg_uuid(GenerateSysId()); sys_cmd.set_user_id("SYSTEM"); sys_cmd.set_receiver(sender); sys_cmd.set_text("LOCAL_CLEAR_CHAT:" + target); sys_cmd.set_type("system_command"); this->PublishToRedis(sys_cmd);
                                            } else if (cmd == "LOGOUT") {
                                                ChatMessage sys_cmd; sys_cmd.set_msg_uuid(GenerateSysId()); sys_cmd.set_user_id("SYSTEM"); sys_cmd.set_receiver(sender); sys_cmd.set_text("FORCE_LOGOUT"); sys_cmd.set_type("system_command"); this->PublishToRedis(sys_cmd);
                                            } else if (cmd == "SEND_PRIVATE") {
                                                std::string target = ai_cmd["target"]; std::string content = ai_cmd["content"];
                                                ChatMessage pmsg; pmsg.set_msg_uuid(GenerateSysId()); pmsg.set_user_id(sender); pmsg.set_receiver(target); pmsg.set_text(content); pmsg.set_type("text");
                                                char tb[64]; time_t n = time(nullptr); strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", localtime(&n)); pmsg.set_timestamp(tb);
                                                this->PublishToRedis(pmsg); SaveMessageToMySQL(pmsg);
                                            } else if (cmd == "SEND_PUBLIC") {
                                                std::string content = ai_cmd["content"];
                                                ChatMessage pmsg; pmsg.set_msg_uuid(GenerateSysId()); pmsg.set_user_id(sender); pmsg.set_receiver("ALL"); pmsg.set_text(content); pmsg.set_type("text");
                                                char tb[64]; time_t n = time(nullptr); strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", localtime(&n)); pmsg.set_timestamp(tb);
                                                this->PublishToRedis(pmsg); SaveMessageToMySQL(pmsg);
                                            }
                                            mysql_close(conn_cmd);
                                        }
                                    }
                                }
                            } catch (...) { /* 静默失败 */ }
                        }
                        
                        ChatMessage end_msg; end_msg.set_msg_uuid(stream_uuid); end_msg.set_user_id("🤖 AI 管家"); end_msg.set_receiver(sender); end_msg.set_text(ai_reply); end_msg.set_type("stream_end");
                        char time_buf[64]; time_t now = time(nullptr); strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now)); end_msg.set_timestamp(time_buf);
                        this->PublishToRedis(end_msg); 
                        
                        end_msg.set_type("text"); SaveMessageToMySQL(end_msg);
                    } catch (...) { std::cerr << "⚠️ AI Core Fault." << std::endl; }
                }).detach();
            } else { PublishToRedis(msg); SaveMessageToMySQL(msg); }
        }
        if (!current_user.empty()) { std::lock_guard<std::mutex> lock(global_mtx); global_clients.erase(current_user); }
        return Status::OK;
    }
};

void RunServer() //gRPC：系统的“迎宾大门”与“通信骨架” 
//负责接收 Node.js 网关发来的指令，并把处理结果返回去。它是整个 C++ 后端的入口。
{
    ServerBuilder builder; 
    builder.AddListeningPort("0.0.0.0:50051", InsecureServerCredentials());
    ChatServiceImpl service; 
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "🚀 CCHAT 4.0 已启动 (全能极简 Agent 版)..." << std::endl;
    server->Wait();
}

int main() { 
    curl_global_init(CURL_GLOBAL_ALL);
    AutoPatchDatabase(); 

    std::thread redis_thread(RedisSubscriber); 
    redis_thread.detach(); 
    RunServer(); 
    curl_global_cleanup();
    return 0; 
}