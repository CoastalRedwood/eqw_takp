#pragma once
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// Utility support functions to simplify reading and storing settings.

namespace Ini {
template <typename T>
T ConvertFromString(const std::string& str, const T& default_value) {
  if constexpr (std::is_same_v<T, bool>) {
    if (str == "TRUE")
      return true;
    else
      return false;
  }
  std::istringstream iss(str);
  T value = default_value;
  iss >> std::boolalpha >> value;
  return value;
}

static inline bool exists(const std::string& section, const std::string& key, const char* filename) {
  char buffer[256];
  DWORD bytes_read = ::GetPrivateProfileStringA(section.c_str(), key.c_str(), "", buffer, sizeof(buffer), filename);

  if (bytes_read == 0) {
    return false;
  }
  return true;
}

static inline std::vector<std::string> GetSectionNames(const char* filename) {
  std::vector<std::string> section_names;
  const DWORD buffer_size = 4096;  // Adjust buffer size as needed
  char buffer[buffer_size];

  DWORD result = ::GetPrivateProfileSectionNamesA(buffer, buffer_size, filename);
  if (result == 0) {
    return section_names;
  }

  for (char* p = buffer; *p != '\0'; p += strlen(p) + 1) {
    section_names.push_back(p);
  }

  return section_names;
}

static inline bool DeleteSection(const std::string& sectionName, const char* filename) {
  // Delete the section and its contents by writing an empty string to it
  if (!::WritePrivateProfileSectionA(sectionName.c_str(), nullptr, filename)) {
    return false;
  } else {
    return true;
  }
}

template <typename T>
T GetValue(std::string section, std::string key, const T& default_value, const char* filename) {
  char buffer[256];
  DWORD bytes_read = ::GetPrivateProfileStringA(section.c_str(), key.c_str(), "", buffer, sizeof(buffer), filename);

  // Write back the default and return it if the entry doesn't exist.
  if (bytes_read == 0) {
    SetValue<T>(section, key, default_value, filename);
    return default_value;
  }
  if constexpr (std::is_same_v<T, std::string>) return buffer;
  return ConvertFromString<T>(std::string(buffer), default_value);
}

template <typename T>
void SetValue(const std::string& section, const std::string& key, const T& value, const char* filename) {
  std::string value_str;
  if constexpr (std::is_same_v<T, bool>) {
    value_str = value ? "TRUE" : "FALSE";
  } else if constexpr (!std::is_same_v<T, std::string>) {
    value_str = std::to_string(value);
  } else {
    value_str = value;
  }
  BOOL result = ::WritePrivateProfileStringA(section.c_str(), key.c_str(), value_str.c_str(), filename);
}
}  // namespace Ini
