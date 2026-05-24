#pragma once
#include <string>

namespace ReadFolderUtils {

bool isInReadFolder(const std::string& path);
std::string buildDestination(const std::string& srcPath);
void moveFinishedBook(const std::string& srcPath, const std::string& dstPath,
                      const std::string& oldCachePath);

}  // namespace ReadFolderUtils
