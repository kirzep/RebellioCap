#include "engine/recording_name.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cwctype>
#include <limits>

namespace rebelliocap {
namespace {
std::wstring wide(const std::string& text){
 if(text.empty())return {};
 if(text.size()>2048)throw std::runtime_error("Name part too long");
 const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
 if(!count)throw std::runtime_error("Invalid UTF-8");
 std::wstring result(static_cast<std::size_t>(count),L'\0');
 MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),count);
 return result;
}
std::wstring digits(unsigned value,unsigned width=2){std::wostringstream s;s<<std::setw(static_cast<int>(width))<<std::setfill(L'0')<<value;return s.str();}
nlohmann::json shadowplay(){return nlohmann::json::array({{{"kind","token"},{"value","game"}},{{"kind","text"},{"value"," "}},{{"kind","token"},{"value","date"}},{{"kind","text"},{"value"," - "}},{{"kind","token"},{"value","time_extended"}},{{"kind","text"},{"value",".DVR"}}});}
}
std::wstring format_recording_name(const nlohmann::json& parts,const NameContext& c){
 if(!parts.is_array()||parts.empty()||parts.size()>64)throw std::runtime_error("Invalid name parts");
 std::wstring result;
 const auto date=digits(c.time.wYear,4)+L"."+digits(c.time.wMonth)+L"."+digits(c.time.wDay);
 const auto time=digits(c.time.wHour)+L"."+digits(c.time.wMinute)+L"."+digits(c.time.wSecond);
 for(const auto& p:parts){
  const auto kind=p.at("kind").get<std::string>(),value=p.at("value").get<std::string>();
  if(kind=="text")result+=wide(value);
  else if(kind!="token")throw std::runtime_error("Unknown name part");
  else if(value=="game")result+=c.game.empty()?L"Desktop":c.game;
  else if(value=="date")result+=date;
  else if(value=="time")result+=time;
  else if(value=="time_extended")result+=time+L"."+digits(c.time.wMilliseconds/10);
  else if(value=="counter")result+=std::to_wstring(c.counter);
  else if(value=="type")result+=c.recording?L"Recording":L"Replay";
  else if(value=="resolution")result+=std::to_wstring(c.width)+L"x"+std::to_wstring(c.height);
  else if(value=="fps")result+=std::to_wstring(c.fps);
  else if(value=="year")result+=digits(c.time.wYear,4);
  else if(value=="month")result+=digits(c.time.wMonth);
  else if(value=="day")result+=digits(c.time.wDay);
  else throw std::runtime_error("Unknown name block");
 }
 for(auto& ch:result)if(ch<32||std::wstring_view(L"<>:\"/\\|?*").find(ch)!=std::wstring_view::npos)ch=L'_';
 if(result.size()>160)result.resize(160);
 // Avoid ending a UTF-16 filename in an unmatched high surrogate.
 if(!result.empty()&&result.back()>=0xD800&&result.back()<=0xDBFF)result.pop_back();
 while(!result.empty()&&(result.back()==L'.'||result.back()==L' '))result.pop_back();
 const auto first=result.find_first_not_of(L" ");result=first==std::wstring::npos?L"Clip":result.substr(first);
 auto stem=result.substr(0,result.find(L'.'));for(auto& ch:stem)ch=static_cast<wchar_t>(std::towupper(ch));
 if(stem==L"CON"||stem==L"PRN"||stem==L"AUX"||stem==L"NUL"||(stem.size()==4&&(stem.starts_with(L"COM")||stem.starts_with(L"LPT"))&&stem[3]>=L'1'&&stem[3]<=L'9'))result=L"_"+result;
 return result;
}
RecordingNames::RecordingNames(std::filesystem::path settings):settings_(std::move(settings)){
 std::ifstream input(settings_.parent_path()/L"recording-counter.txt");input>>counter_;
 persisted_counter_=counter_;
 reload();
}
void RecordingNames::reload(){
 auto parts=shadowplay();
 try{
  std::error_code ec;const auto bytes=std::filesystem::file_size(settings_,ec);
  if(!ec&&bytes<=131072){std::ifstream input(settings_);const auto settings=nlohmann::json::parse(input);for(const auto& preset:settings.at("presets"))if(preset.at("id")==settings.at("activeId")){parts=preset.at("parts");break;}}
  static_cast<void>(format_recording_name(parts,NameContext{}));
 }catch(...){parts=shadowplay();}
 std::scoped_lock lock(mutex_);
 parts_=std::move(parts);
}
std::wstring RecordingNames::capture(NameContext context){
 std::scoped_lock lock(mutex_);
 if(counter_!=(std::numeric_limits<std::uint64_t>::max)())++counter_;
 context.counter=counter_;
 return format_recording_name(parts_,context);
}
void RecordingNames::persist_counter() noexcept {
 try {
  // Serialize disk writes separately: capture never waits for the disk lock.
  std::scoped_lock persistence_lock(persistence_mutex_);
  std::uint64_t counter;
  {std::scoped_lock lock(mutex_);counter=counter_;}
  if(counter==persisted_counter_)return;
  const auto counter_file=settings_.parent_path()/L"recording-counter.txt";
  const auto temp=settings_.parent_path()/L"recording-counter.tmp";
  {std::ofstream output(temp,std::ios::trunc);output<<counter;output.flush();if(!output)return;}
  if(MoveFileExW(temp.c_str(),counter_file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))persisted_counter_=counter;
 }catch(...){/* Counter persistence has always been best effort. */}
}
}
