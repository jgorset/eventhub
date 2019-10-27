#ifndef EVENTHUB_CONFIG_HPP
#define EVENTHUB_CONFIG_HPP

#include <string>

namespace eventhub {
class EventhubConfig {
public:
  static EventhubConfig& getInstance() {
    static EventhubConfig instance;
    return instance;
  }

  std::string getJWTSecret() {
    return "eventhub_secret";
  }
};
} // namespace eventhub

#define Config EventhubConfig::getInstance()

#endif