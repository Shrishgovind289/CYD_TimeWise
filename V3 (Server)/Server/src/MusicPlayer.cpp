#include "MusicPlayer.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>

MusicPlayer::MusicPlayer()=default;
MusicSong MusicPlayer::select(const std::string& action) {
    if(action!="random" && action!="next" && action!="previous")
        throw std::invalid_argument("invalid_music_action");
    std::lock_guard<std::mutex> lock(mutex_);
    if(action=="previous" && !history_.empty()) {
        if(cursor_>0) --cursor_;
        return history_.at(cursor_);
    }
    if(action=="next" && cursor_+1<history_.size()) return history_.at(++cursor_);
    if(action=="previous") throw std::runtime_error("no_previous_track");
    MusicSong song=navidrome_.randomSong();
    if(cursor_+1<history_.size()) history_.resize(cursor_+1);
    history_.push_back(song);
    if(history_.size()>50) history_.erase(history_.begin());
    cursor_=history_.size()-1;
    return song;
}
bool MusicPlayer::isSelected(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !history_.empty() && history_.at(cursor_).id==id;
}
void MusicPlayer::stream(const std::string& id,const std::function<bool(const char*,size_t)>& emit) {
    if(!isSelected(id)) throw std::runtime_error("song_not_selected");
    std::string url=navidrome_.streamUrl(id);
    int fds[2]; if(pipe(fds)!=0) throw std::runtime_error("pipe_failed");
    pid_t pid=fork();
    if(pid<0){close(fds[0]);close(fds[1]);throw std::runtime_error("fork_failed");}
    if(pid==0){
        dup2(fds[1],STDOUT_FILENO);
        close(fds[0]);close(fds[1]);
        execlp("ffmpeg","ffmpeg","-nostdin","-hide_banner","-loglevel","error",
               "-i",url.c_str(),"-vn","-ac","1","-ar","24000",
               "-f","u8","-acodec","pcm_u8","pipe:1",(char*)nullptr);
        _exit(127);
    }
    close(fds[1]);
    std::array<char,1024> buffer{};
    auto next=std::chrono::steady_clock::now();
    while(true){
        ssize_t bytes=read(fds[0],buffer.data(),buffer.size());
        if(bytes<0 && errno==EINTR)continue;
        if(bytes<=0)break;
        if(!emit(buffer.data(),static_cast<size_t>(bytes)))break;
        next+=std::chrono::microseconds((1000000LL*bytes)/24000);
        std::this_thread::sleep_until(next);
    }
    close(fds[0]);
    kill(pid,SIGTERM);
    int status=0;
    while(waitpid(pid,&status,0)<0 && errno==EINTR){}
}
