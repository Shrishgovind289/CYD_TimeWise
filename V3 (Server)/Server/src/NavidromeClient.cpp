#include "NavidromeClient.hpp"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <array>
#include <cstdlib>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace {
std::string requiredEnv(const char* name) {
    const char* value=std::getenv(name);
    if (!value || !*value) throw std::runtime_error(std::string("missing_environment:")+name);
    return value;
}
std::string hexEncode(const unsigned char* p, size_t n) {
    std::ostringstream out;
    for(size_t i=0;i<n;i++) out<<std::hex<<std::setfill('0')<<std::setw(2)<<(unsigned)p[i];
    return out.str();
}
std::string escape(const std::string& input) {
    CURL* c=curl_easy_init();
    if (!c) throw std::runtime_error("curl_init_failed");
    char* value=curl_easy_escape(c,input.c_str(),static_cast<int>(input.size()));
    if (!value) {curl_easy_cleanup(c);throw std::runtime_error("url_encode_failed");}
    std::string result(value);
    curl_free(value); curl_easy_cleanup(c);
    return result;
}
size_t onData(char* ptr,size_t size,size_t nmemb,void* user) {
    auto* dest=static_cast<std::string*>(user);
    size_t count=size*nmemb;
    if(dest->size()+count>1024*1024) return 0;
    dest->append(ptr,count);
    return count;
}
}
NavidromeClient::NavidromeClient():base_(requiredEnv("TIMEWISE_NAVIDROME_URL")),
    user_(requiredEnv("TIMEWISE_NAVIDROME_USER")),pass_(requiredEnv("TIMEWISE_NAVIDROME_PASSWORD")) {
    if(base_.back()=='/') base_.pop_back();
    if(base_.rfind("http://",0)!=0 && base_.rfind("https://",0)!=0)
        throw std::runtime_error("invalid_navidrome_url");
}
std::string NavidromeClient::apiUrl(const std::string& endpoint) const {
    std::random_device rd;
    std::array<unsigned char,8> salt{};
    for(auto& b:salt) b=static_cast<unsigned char>(rd());
    std::string saltHex=hexEncode(salt.data(),salt.size());
    std::string passwordAndSalt=pass_+saltHex;
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned length=0;
    if(!EVP_Digest(passwordAndSalt.data(),passwordAndSalt.size(),digest,&length,EVP_md5(),nullptr))
        throw std::runtime_error("md5_failed");
    return base_+"/rest/"+endpoint+"?u="+escape(user_)+"&t="+
        hexEncode(digest,length)+"&s="+saltHex+"&v=1.16.1&c=TimeWiseGateway";
}
MusicSong NavidromeClient::randomSong() const {
    std::string url=apiUrl("getRandomSongs.view")+"&f=json&size=1";
    CURL* curl=curl_easy_init();
    if(!curl) throw std::runtime_error("curl_init_failed");
    std::string body;
    curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,onData);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,&body);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,5L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,12L);
    curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
    CURLcode result=curl_easy_perform(curl);
    long status=0; curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
    curl_easy_cleanup(curl);
    if(result!=CURLE_OK || status!=200) throw std::runtime_error("navidrome_request_failed");
    auto data=nlohmann::json::parse(body);
    auto& response=data.at("subsonic-response");
    if(response.value("status",std::string{})!="ok") throw std::runtime_error("navidrome_api_error");
    auto& songs=response.at("randomSongs").at("song");
    if(!songs.is_array() || songs.empty()) throw std::runtime_error("no_songs");
    const auto& song=songs.at(0);
    MusicSong selected;
    selected.id=song.at("id").get<std::string>();
    selected.title=song.value("title",std::string("Unknown"));
    selected.artist=song.value("artist",std::string("Unknown"));
    selected.album=song.value("album",std::string("Unknown"));
    selected.duration_seconds=song.value("duration",0U);
    return selected;
}
std::string NavidromeClient::streamUrl(const std::string& id) const {
    return apiUrl("stream.view")+"&id="+escape(id);
}
