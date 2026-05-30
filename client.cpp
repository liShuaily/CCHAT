#include <iostream>
#include <memory>
#include <string>
#include <thread> // 【新增】C++11 标准线程库
#include <grpcpp/grpcpp.h>
#include "chat.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using grpc::ClientReaderWriter; // 【新增】用于双向流的读写器
using chat::LoginRequest;
using chat::LoginResponse;
using chat::ChatMessage;        // 【新增】聊天消息体
using chat::ChatService;

class ChatClient {
public:
    ChatClient(std::shared_ptr<Channel> channel) : stub_(ChatService::NewStub(channel)) {}

    // 登录模块 (保持不变)
    bool Login(const std::string& user, const std::string& pass) {
        LoginRequest request;
        request.set_user_id(user);
        request.set_password(pass);
        LoginResponse reply;
        ClientContext context;

        Status status = stub_->Login(&context, request, &reply);
        if (status.ok() && reply.success()) {
            std::cout << "✅ 登录成功！" << std::endl;
            return true;
        } else {
            std::cout << "❌ 登录失败: " << reply.message() << std::endl;
            return false;
        }
    }

    // 【全新加入】进入双向流聊天室
    void StartChat(const std::string& user_id) {
        ClientContext context;
        // 建立双向流管道
        std::shared_ptr<ClientReaderWriter<ChatMessage, ChatMessage>> stream(stub_->ChatStream(&context));

        // 核心架构：开辟一个独立的后台子线程，专门用来“听”服务器发来的消息
        // 这样它就不会卡住我们主线程在键盘上的输入
        std::thread receiver_thread([stream]() {
            ChatMessage server_msg;
            while (stream->Read(&server_msg)) {
                std::cout << "\n[" << server_msg.user_id() << "]: " << server_msg.text() << std::endl;
                std::cout << "请输入: " << std::flush; // 重新打印输入提示符
            }
        });

        // 主线程：专门负责从键盘读取输入，并“发”给服务器
        std::string input;
        std::cout << "\n=== 成功进入 CCHAT 聊天大厅 ===" << std::endl;
        std::cout << "请输入你想说的话 (输入 'quit' 退出):" << std::endl;
        
        while (true) {
            std::cout << "请输入: ";
            std::getline(std::cin, input);
            if (input == "quit") {
                break;
            }
            if (input.empty()) continue;

            ChatMessage msg;
            msg.set_user_id(user_id);
            msg.set_text(input);
            msg.set_timestamp("now");
            
            // 把消息写进管道，发给服务器
            stream->Write(msg);
        }

        // 告诉服务器：我不发了，准备断开
        stream->WritesDone();
        
        // 等待那个“听”的子线程安全结束
        receiver_thread.join();
        Status status = stream->Finish();
        std::cout << "已退出聊天室。" << std::endl;
    }

private:
    std::unique_ptr<ChatService::Stub> stub_;
};

int main() {
    ChatClient client(grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials()));
    
    std::string current_user = "shuai";
    std::string password = "888888"; // 之前存入 Redis 的密码

    // 1. 先进行身份验证
    if (client.Login(current_user, password)) {
        // 2. 身份通过后，正式激活双向流进入聊天室
        client.StartChat(current_user);
    }
    
    return 0;
}