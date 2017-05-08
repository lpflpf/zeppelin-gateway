#include "slash/include/slash_string.h"
#include "zgw_store.h"

#include <iostream>
#include <chrono>
#include <thread>

namespace zgwstore {

ZgwStore::ZgwStore(const std::string& lock_name, const int32_t lock_ttl) :
  zp_cli_(nullptr), redis_cli_(nullptr), redis_ip_(""), redis_port_(-1),
  lock_name_(lock_name), lock_ttl_(lock_ttl), redis_error_(false) {
  };

ZgwStore::~ZgwStore() {
  if (zp_cli_ != nullptr) {
    delete zp_cli_;
  }
  if (redis_cli_ != nullptr) {
    redisFree(redis_cli_);
  }
}

Status ZgwStore::Open(const std::vector<std::string>& zp_addrs,
    const std::string& redis_addr, const std::string& lock_name,
    const int32_t lock_ttl, ZgwStore** store) {

  *store = nullptr;
/*
 * Connect to zeppelin
 */
  if (zp_addrs.empty()) {
    return Status::InvalidArgument("Invalid zeppelin addresses");
  }

  std::string t_ip;
  int t_port = 0;
  Status s;
  libzp::Options zp_option;
  for (auto& addr : zp_addrs) {
    if (!slash::ParseIpPortString(addr, t_ip, t_port)) {
      return Status::InvalidArgument("Invalid zeppelin address");
    }
    zp_option.meta_addr.push_back(libzp::Node(t_ip, t_port));
  }
  libzp::Cluster* zp_cli = new libzp::Cluster(zp_option);
  s = zp_cli->Connect();
  if (!s.ok()) {
    delete zp_cli;
    return Status::IOError("Failed to connect to zeppelin");
  }
/*
 *  Connect to redis
 */
  if (!slash::ParseIpPortString(redis_addr, t_ip, t_port)) {
    delete zp_cli;
    return Status::InvalidArgument("Invalid zeppelin address");
  }
  redisContext* redis_cli;

  struct timeval timeout = { 1, 500000 }; // 1.5 seconds
  redis_cli = redisConnectWithTimeout(t_ip.c_str(), t_port, timeout);
  if (redis_cli == NULL || redis_cli->err) {
    delete zp_cli;
    if (redis_cli) {
      redisFree(redis_cli);
      return Status::IOError("Failed to connect to redis");
    } else {
      return Status::Corruption("Connection error: can't allocate redis context");
    }
  }
  
  *store = new ZgwStore(lock_name, lock_ttl);
  (*store)->InstallClients(zp_cli, redis_cli);
  (*store)->set_redis_ip(t_ip);
  (*store)->set_redis_port(t_port);
  return Status::OK();
}

void ZgwStore::InstallClients(libzp::Cluster* zp_cli, redisContext* redis_cli) {
  zp_cli_ = zp_cli;
  redis_cli_ = redis_cli;
}

Status ZgwStore::AddUser(const User& user) {
  if (!MaybeHandleRedisError()) {
    return Status::IOError("Reconnect");
  }
  
  std::string add_cmd = "HMSET " + kZgwUserPrefix + user.display_name;
  add_cmd += (" uid " + user.user_id);
  add_cmd += (" name " + user.display_name);
  for (auto& iter : user.key_pairs) {
    add_cmd += (" " + iter.first + " " + iter.second);
  }

  Status s;
  s = Lock();
  if (!s.ok()) {
    return s;
  }
  
  redisReply *reply;
  reply = static_cast<redisReply*>(redisCommand(redis_cli_,
              "SADD %s %s", kZgwUserList.c_str(), user.display_name.c_str()));
  if (reply == NULL) {
    return HandleIOError("AddUser::SADD");
  }

  if (reply->type == REDIS_REPLY_ERROR) {
    return HandleLogicError("AddUser::SADD ret: " + std::string(reply->str), reply, true);
  }

  if (reply->integer == 0) {
    return HandleLogicError("User Already Exist", reply, true);
  }

  assert(reply->integer == 1);

  freeReplyObject(reply);
  reply = static_cast<redisReply*>(redisCommand(redis_cli_, "DEL %s%s",
              kZgwUserPrefix.c_str(), user.display_name.c_str()));
  if (reply == NULL) {
    return HandleIOError("AddUser::DEL");
  }

  freeReplyObject(reply);
  reply = static_cast<redisReply*>(redisCommand(redis_cli_, add_cmd.c_str()));
  if (reply == NULL) {
    return HandleIOError("AddUser::HMSET");
  }
  if (reply->type == REDIS_REPLY_ERROR) {
    return HandleLogicError("AddUser::HMSET ret: " + std::string(reply->str), reply, true);
  }

  freeReplyObject(reply);
  s = UnLock();
  return s;
}

Status ZgwStore::ListUsers(std::vector<User>& users) {
  users.clear();
  if (!MaybeHandleRedisError()) {
    return Status::IOError("Reconnect");
  }

  redisReply *reply;
  reply = static_cast<redisReply*>(redisCommand(redis_cli_,
              "SMEMBERS %s", kZgwUserList.c_str()));

  if (reply == NULL) {
    return HandleIOError("ListUsers::SEMBMBERS");
  }
  if (reply->type == REDIS_REPLY_ERROR) {
    return HandleLogicError("ListUser::SMEMBERS ret: " + std::string(reply->str), reply, false);
  }
  
  assert(reply->type == REDIS_REPLY_ARRAY);
  
  if (reply->elements == 0) {
    return Status::OK();
  }

  redisReply* t_reply;
  for (unsigned int i = 0; i < reply->elements; i++) {
    t_reply = static_cast<redisReply*>(redisCommand(redis_cli_, "HGETALL %s%s",
          kZgwUserPrefix.c_str(), reply->element[i]->str));
    if (t_reply == NULL) {
      freeReplyObject(reply);
      return HandleIOError("ListUsers::HGETALL");
    }
    if (t_reply->type == REDIS_REPLY_ERROR) {
      freeReplyObject(reply);
      return HandleLogicError("ListUser::HGETALL ret: " + std::string(t_reply->str), t_reply, false);
    }

    assert(t_reply->type == REDIS_REPLY_ARRAY);
    if (t_reply->elements == 0) {
      continue;
    } else if (t_reply->elements % 2 != 0) {
      freeReplyObject(reply);
      return HandleLogicError("ListUser::HGETALL: elements % 2 != 0", t_reply, false);
    }
    users.push_back(GenUserFromReply(t_reply));
    freeReplyObject(t_reply);
  }

  freeReplyObject(reply);
  return Status::OK();
}

bool ZgwStore::MaybeHandleRedisError() {
  if (!redis_error_) {
    return true;
  }

  struct timeval timeout = { 1, 500000 }; // 1.5 seconds
  redis_cli_ = redisConnectWithTimeout(redis_ip_.c_str(), redis_port_, timeout);
  if (redis_cli_ == NULL || redis_cli_->err) {
    if (redis_cli_) {
      redisFree(redis_cli_);
    }
    return false;
  }
  redis_error_ = true;
  return true;
}

Status ZgwStore::HandleIOError(const std::string& func_name) {
  redisFree(redis_cli_);
  redis_error_ = true;
  return Status::IOError(func_name);
}

Status ZgwStore::HandleLogicError(const std::string& str_err, redisReply* reply,
    const bool should_unlock) {
  freeReplyObject(reply);
  Status s;
  if (should_unlock) {
    s = UnLock();
    return Status::Corruption(str_err + ", UnLock ret: " + s.ToString());
  }
  return Status::Corruption(str_err);
}

User ZgwStore::GenUserFromReply(redisReply* reply) {
  User user;
  for (unsigned int i = 0; i < reply->elements; i++) {
    if (std::string(reply->element[i]->str) == "uid") {
      user.user_id = reply->element[++i]->str;
      continue;
    } else if (std::string(reply->element[i]->str) == "name") {
      user.display_name = reply->element[++i]->str;
      continue;
    } else {
      user.key_pairs.insert(std::pair<std::string, std::string>(reply->element[i]->str,
            reply->element[++i]->str));
      continue;
    }
  }
  return user;
}

Status ZgwStore::Lock() {
  if (!MaybeHandleRedisError()) {
    return Status::IOError("Reconnect");
  }

  redisReply *reply;
  while (true) {
    reply = static_cast<redisReply*>(redisCommand(redis_cli_,
                "SET zgw_lock %s NX PX %lu", lock_name_.c_str(), lock_ttl_));
    if (reply == NULL) {
      return HandleIOError("Lock");
    }
    if (reply->type == REDIS_REPLY_STATUS && !strcmp(reply->str, "OK")) {
      freeReplyObject(reply);
      break;
    }
    freeReplyObject(reply);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  return Status::OK();
}

Status ZgwStore::UnLock() {
  if (!MaybeHandleRedisError()) {
    return Status::IOError("Reconnect");
  }

  std::string del_cmd = "if redis.call(\"get\", \"zgw_lock\") == \"" + lock_name_ + "\" "
                        "then "
                        "return redis.call(\"del\", \"zgw_lock\") "
                        "else "
                        "return 0 "
                        "end ";

  redisReply *reply;
  reply = static_cast<redisReply*>(redisCommand(redis_cli_,
              "EVAL %s %d", del_cmd.c_str(), 0));
  if (reply == NULL) {
    return HandleIOError("UnLock");
  }
  if (reply->integer == 1) {
    // UnLock Success
  } else if (reply->integer == 0) {
    // The zgw_lock is held by other clients
  }
  freeReplyObject(reply);
  return Status::OK();
}

}
