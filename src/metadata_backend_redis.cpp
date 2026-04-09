/*
 * s3fs - FUSE-based file system backed by Amazon S3
 *
 * Copyright(C) 2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "metadata_backend.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/time.h>
#include <utility>

#ifdef HAVE_HIREDIS
#include <hiredis/hiredis.h>
#endif

namespace {

#ifdef HAVE_HIREDIS

struct RedisEndpoint
{
    std::string host;
    int         port = 6379;
    int         db = 0;
};

static bool ParseIntInRange(const std::string& input, int min_value, int max_value, int& parsed)
{
    if(input.empty()){
        return false;
    }

    errno      = 0;
    char* end  = nullptr;
    long value = std::strtol(input.c_str(), &end, 10);
    if(errno != 0 || end == input.c_str() || *end != '\0'){
        return false;
    }
    if(value < min_value || value > max_value){
        return false;
    }

    parsed = static_cast<int>(value);
    return true;
}

static bool ParseUint64(const char* input, uint64_t& parsed)
{
    if(!input || *input == '\0'){
        return false;
    }
    if(*input == '-'){
        return false;
    }

    errno                    = 0;
    char* end                = nullptr;
    unsigned long long value = std::strtoull(input, &end, 10);
    if(errno != 0 || end == input || *end != '\0'){
        return false;
    }

    parsed = static_cast<uint64_t>(value);
    return true;
}

static bool ParseRedisUri(const std::string& redis_uri, RedisEndpoint& endpoint)
{
    static const std::string scheme("redis://");
    if(redis_uri.size() <= scheme.size() || 0 != redis_uri.compare(0, scheme.size(), scheme)){
        return false;
    }

    std::string body = redis_uri.substr(scheme.size());
    const std::string::size_type query_pos = body.find('?');
    if(query_pos != std::string::npos){
        body.erase(query_pos);
    }
    if(body.empty()){
        return false;
    }

    std::string authority;
    std::string db_part;
    const std::string::size_type slash_pos = body.find('/');
    if(slash_pos == std::string::npos){
        authority = body;
    }else{
        authority = body.substr(0, slash_pos);
        db_part   = body.substr(slash_pos + 1);
    }

    if(authority.empty()){
        return false;
    }

    const std::string::size_type auth_pos = authority.rfind('@');
    if(auth_pos != std::string::npos){
        authority = authority.substr(auth_pos + 1);
    }
    if(authority.empty()){
        return false;
    }

    std::string host;
    std::string port_part;
    if(authority.front() == '['){
        const std::string::size_type closing = authority.find(']');
        if(closing == std::string::npos){
            return false;
        }
        host = authority.substr(1, closing - 1);
        if((closing + 1) < authority.size()){
            if(authority[closing + 1] != ':'){
                return false;
            }
            port_part = authority.substr(closing + 2);
        }
    }else{
        const std::string::size_type first_colon = authority.find(':');
        const std::string::size_type last_colon  = authority.rfind(':');
        if(first_colon != std::string::npos && first_colon == last_colon){
            host      = authority.substr(0, first_colon);
            port_part = authority.substr(first_colon + 1);
        }else{
            host = authority;
        }
    }

    if(host.empty()){
        return false;
    }

    endpoint.host = host;

    if(!port_part.empty()){
        int parsed_port = 0;
        if(!ParseIntInRange(port_part, 1, 65535, parsed_port)){
            return false;
        }
        endpoint.port = parsed_port;
    }

    if(!db_part.empty()){
        const std::string::size_type next_slash = db_part.find('/');
        if(next_slash != std::string::npos){
            db_part.erase(next_slash);
        }

        if(!db_part.empty()){
            int parsed_db = 0;
            if(!ParseIntInRange(db_part, 0, INT_MAX, parsed_db)){
                return false;
            }
            endpoint.db = parsed_db;
        }
    }

    return true;
}

static timeval ToTimeval(long timeout_ms)
{
    if(timeout_ms <= 0){
        timeout_ms = 100;
    }

    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return tv;
}

class RedisMetadataBackend final : public MetadataBackend
{
public:
    RedisMetadataBackend(RedisEndpoint redis_endpoint, const MetadataBackendConfig& config) :
        endpoint(std::move(redis_endpoint)),
        used_bytes_key(config.used_bytes_key.empty() ? "s3fs:capacity:used_bytes" : config.used_bytes_key),
        connect_timeout_ms(config.connect_timeout_ms),
        rw_timeout_ms(config.rw_timeout_ms),
        redis_ctx(nullptr),
        used_bytes(0)
    {
    }

    ~RedisMetadataBackend() override
    {
        const std::lock_guard<std::mutex> lock(redis_lock);
        DisconnectLocked();
    }

    bool Initialize()
    {
        const std::lock_guard<std::mutex> lock(redis_lock);
        return ConnectLocked();
    }

    std::string Name() const override
    {
        return "redis";
    }

    uint64_t GetUsedBytes() const override
    {
        return used_bytes.load(std::memory_order_relaxed);
    }

    bool AddUsedBytesDelta(int64_t delta) override
    {
        if(delta == 0){
            return true;
        }

        const std::lock_guard<std::mutex> lock(redis_lock);
        for(int retry = 0; retry < 2; ++retry){
            if(!EnsureConnectedLocked()){
                continue;
            }

            uint64_t updated = 0;
            if(ApplyDeltaLocked(delta, updated)){
                used_bytes.store(updated, std::memory_order_relaxed);
                return true;
            }

            DisconnectLocked();
        }
        return false;
    }

private:
    bool EnsureConnectedLocked()
    {
        if(redis_ctx && redis_ctx->err == 0){
            return true;
        }
        DisconnectLocked();
        return ConnectLocked();
    }

    bool ConnectLocked()
    {
        const timeval connect_timeout = ToTimeval(connect_timeout_ms);
        redis_ctx                     = redisConnectWithTimeout(endpoint.host.c_str(), endpoint.port, connect_timeout);
        if(!redis_ctx){
            return false;
        }
        if(redis_ctx->err != 0){
            DisconnectLocked();
            return false;
        }

        const timeval rw_timeout = ToTimeval(rw_timeout_ms);
        if(redisSetTimeout(redis_ctx, rw_timeout) != REDIS_OK){
            DisconnectLocked();
            return false;
        }

        if(endpoint.db != 0){
            redisReply* reply = static_cast<redisReply*>(redisCommand(redis_ctx, "SELECT %d", endpoint.db));
            if(!reply){
                DisconnectLocked();
                return false;
            }

            const bool ok = (reply->type == REDIS_REPLY_STATUS && reply->str && std::strcmp(reply->str, "OK") == 0);
            freeReplyObject(reply);
            if(!ok){
                DisconnectLocked();
                return false;
            }
        }

        if(!EnsureCounterKeyLocked()){
            DisconnectLocked();
            return false;
        }

        uint64_t current = 0;
        if(!RefreshUsedBytesLocked(current)){
            DisconnectLocked();
            return false;
        }
        used_bytes.store(current, std::memory_order_relaxed);

        return true;
    }

    void DisconnectLocked()
    {
        if(redis_ctx){
            redisFree(redis_ctx);
            redis_ctx = nullptr;
        }
    }

    bool EnsureCounterKeyLocked()
    {
        redisReply* reply = static_cast<redisReply*>(redisCommand(redis_ctx, "SETNX %b 0", used_bytes_key.data(), used_bytes_key.size()));
        if(!reply){
            return false;
        }

        const bool ok = (reply->type == REDIS_REPLY_INTEGER || reply->type == REDIS_REPLY_STATUS);
        freeReplyObject(reply);
        return ok;
    }

    bool ParseUsedBytesReply(const redisReply* reply, uint64_t& parsed) const
    {
        if(!reply){
            return false;
        }

        switch(reply->type){
            case REDIS_REPLY_NIL:
                parsed = 0;
                return true;
            case REDIS_REPLY_INTEGER:
                if(reply->integer < 0){
                    parsed = 0;
                    return true;
                }
                parsed = static_cast<uint64_t>(reply->integer);
                return true;
            case REDIS_REPLY_STRING:
            case REDIS_REPLY_STATUS:
                return ParseUint64(reply->str, parsed);
            default:
                return false;
        }
    }

    bool RefreshUsedBytesLocked(uint64_t& current)
    {
        redisReply* reply = static_cast<redisReply*>(redisCommand(redis_ctx, "GET %b", used_bytes_key.data(), used_bytes_key.size()));
        if(!reply){
            return false;
        }

        if(ParseUsedBytesReply(reply, current)){
            freeReplyObject(reply);
            return true;
        }
        freeReplyObject(reply);

        redisReply* reset = static_cast<redisReply*>(redisCommand(redis_ctx, "SET %b 0", used_bytes_key.data(), used_bytes_key.size()));
        if(!reset){
            return false;
        }

        const bool ok = (reset->type == REDIS_REPLY_STATUS && reset->str && std::strcmp(reset->str, "OK") == 0);
        freeReplyObject(reset);
        if(ok){
            current = 0;
        }
        return ok;
    }

    bool ApplyDeltaLocked(int64_t delta, uint64_t& updated)
    {
        static const char* update_script =
            "local value = redis.call('INCRBY', KEYS[1], ARGV[1]) "
            "if value < 0 then "
            "  redis.call('SET', KEYS[1], '0') "
            "  return 0 "
            "end "
            "return value";

        redisReply* reply = static_cast<redisReply*>(
            redisCommand(redis_ctx, "EVAL %s 1 %b %lld", update_script, used_bytes_key.data(), used_bytes_key.size(), static_cast<long long>(delta)));
        if(!reply){
            return false;
        }

        if(reply->type == REDIS_REPLY_ERROR){
            freeReplyObject(reply);
            return false;
        }

        const bool ok = ParseUsedBytesReply(reply, updated);
        freeReplyObject(reply);
        return ok;
    }

private:
    RedisEndpoint               endpoint;
    std::string                 used_bytes_key;
    long                        connect_timeout_ms;
    long                        rw_timeout_ms;
    mutable std::mutex          redis_lock;
    redisContext*               redis_ctx;
    std::atomic<uint64_t>       used_bytes;
};

#endif // HAVE_HIREDIS

} // namespace

MetadataBackendPtr CreateRedisMetadataBackend(const MetadataBackendConfig& config)
{
#ifdef HAVE_HIREDIS
    RedisEndpoint endpoint;
    if(!ParseRedisUri(config.redis_uri, endpoint)){
        return nullptr;
    }

    auto backend = std::make_unique<RedisMetadataBackend>(std::move(endpoint), config);
    if(!backend->Initialize()){
        return nullptr;
    }
    return backend;
#else
    (void)config;
    return nullptr;
#endif
}

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
