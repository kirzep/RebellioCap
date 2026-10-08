#include <Windows.h>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <future>
#include <thread>
#include "engine/session_input.h"
using namespace rebelliocap;
using namespace std::chrono_literals;
namespace {
struct Pipe {
 HANDLE read{}, write{};
 Pipe(){if(!CreatePipe(&read,&write,nullptr,256*1024))throw std::runtime_error("CreatePipe failed");}
 ~Pipe(){if(read)CloseHandle(read);if(write)CloseHandle(write);}
 void send(const std::string& bytes){DWORD written=0;REQUIRE(WriteFile(write,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr));REQUIRE(written==bytes.size());}
};
}
TEST_CASE("session input blocks idle and wakes on input and EOF") {
 Pipe pipe; auto wake=std::make_shared<SessionWake>(); SessionPipeReader reader(pipe.read,wake);
 const auto revision=wake->revision();
 const auto start=std::chrono::steady_clock::now();
 REQUIRE_FALSE(wake->wait_until(revision,start+100ms));
 REQUIRE(std::chrono::steady_clock::now()-start>=90ms);
 REQUIRE_FALSE(reader.try_take().has_value());
 pipe.send("request\n");
 REQUIRE(wake->wait_until(revision,std::chrono::steady_clock::now()+2s));
 auto chunk=reader.try_take();REQUIRE(chunk.has_value());REQUIRE(chunk->bytes=="request\n");REQUIRE_FALSE(chunk->eof);
 const auto before_eof=wake->revision();CloseHandle(pipe.write);pipe.write=nullptr;
 REQUIRE(wake->wait_until(before_eof,std::chrono::steady_clock::now()+2s));
 auto eof=reader.try_take();REQUIRE(eof.has_value());REQUIRE(eof->eof);REQUIRE_FALSE(eof->error.has_value());
}
TEST_CASE("session input cancellation cannot miss reader startup or blocked reads") {
 Pipe pipe;auto wake=std::make_shared<SessionWake>();
 const auto start=std::chrono::steady_clock::now();
 for(int i=0;i<40;++i){SessionPipeReader reader(pipe.read,wake);if(i%2)std::this_thread::sleep_for(1ms);}
 REQUIRE(std::chrono::steady_clock::now()-start<5s);
}
TEST_CASE("session wake preserves notifications arriving during controller work") {
 SessionWake wake;
 const auto before_work=wake.revision();
 wake.notify();
 const auto start=std::chrono::steady_clock::now();
 REQUIRE(wake.wait_until(before_work,start+1s));
 REQUIRE(std::chrono::steady_clock::now()-start<100ms);
 REQUIRE_FALSE(wake.wait_until(wake.revision(),std::chrono::steady_clock::now()+10ms));
}
TEST_CASE("session reader preserves fragmented bytes before EOF") {
 Pipe pipe;auto wake=std::make_shared<SessionWake>();SessionPipeReader reader(pipe.read,wake);
 pipe.send("{\"request");pipe.send("Id\":\"1\"}\n");
 CloseHandle(pipe.write);pipe.write=nullptr;
 std::string received;bool eof=false;
 const auto deadline=std::chrono::steady_clock::now()+2s;
 while(!eof&&std::chrono::steady_clock::now()<deadline){
  const auto revision=wake->revision();
  while(auto chunk=reader.try_take()){REQUIRE_FALSE(chunk->error.has_value());received+=chunk->bytes;eof=chunk->eof;}
  if(!eof)wake->wait_until(revision,deadline);
 }
 REQUIRE(eof);REQUIRE(received=="{\"requestId\":\"1\"}\n");
}
TEST_CASE("session input bounds its queued chunks and cancels backpressure") {
 Pipe pipe;auto wake=std::make_shared<SessionWake>();
 const auto start=std::chrono::steady_clock::now();
 {
  SessionPipeReader reader(pipe.read,wake);pipe.send(std::string(34*4096,'x'));
  const auto end=std::chrono::steady_clock::now()+2s;
  while(reader.queued_chunks()<32&&std::chrono::steady_clock::now()<end)std::this_thread::yield();
  REQUIRE(reader.queued_chunks()==32);
 }
 REQUIRE(std::chrono::steady_clock::now()-start<5s);
}
