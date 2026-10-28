#pragma once
#include <string>
namespace dh {
void management_start();
bool update_public_key_valid(const std::string &pem);
std::string update_key_fingerprint(const std::string &pem);
}
