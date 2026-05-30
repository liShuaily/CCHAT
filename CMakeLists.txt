cmake_minimum_required(VERSION 3.15)
project(ChatSystem CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(Threads REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# 寻找数据库驱动
find_library(HIREDIS_LIB hiredis)
find_library(MYSQL_LIB mysqlclient)

# ==========================================
# 🌟 核心大招：全自动 Proto 编译流水线
# ==========================================
# 1. 锁定你的 Proto 文件位置
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_FILE "${PROTO_DIR}/chat.proto")

# 2. 告诉 CMake 生成的 C++ 代码要放在哪里 (统一放进 build 目录，保持根目录纯净)
set(PROTO_SRC "${CMAKE_CURRENT_BINARY_DIR}/chat.pb.cc")
set(PROTO_HDR "${CMAKE_CURRENT_BINARY_DIR}/chat.pb.h")
set(GRPC_SRC "${CMAKE_CURRENT_BINARY_DIR}/chat.grpc.pb.cc")
set(GRPC_HDR "${CMAKE_CURRENT_BINARY_DIR}/chat.grpc.pb.h")

# 3. 设定触发器：只要 chat.proto 被修改，自动执行 protoc 编译！
add_custom_command(
    OUTPUT "${PROTO_SRC}" "${PROTO_HDR}" "${GRPC_SRC}" "${GRPC_HDR}"
    COMMAND protobuf::protoc
    ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
         --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
         -I "${PROTO_DIR}"
         --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
         "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
)

# ==========================================
# 编译 Server
# ==========================================
# 把自动生成的 .cc 文件加入到编译队伍中
add_executable(server server.cpp ${PROTO_SRC} ${GRPC_SRC})

# 告诉 C++ 编译器：生成的 .pb.h 头文件去 build 目录里找！
target_include_directories(server PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(server
    gRPC::grpc++             
    protobuf::libprotobuf    
    Threads::Threads
    ${HIREDIS_LIB}
    ${MYSQL_LIB}
    curl
)