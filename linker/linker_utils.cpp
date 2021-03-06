/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "linker_utils.h"

#include "linker_debug.h"
#include "linker_globals.h"

#include "android-base/strings.h"

#include <sys/stat.h>
#include <unistd.h>

std::string dirname(const char* path) {
  const char* last_slash = strrchr(path, '/');

  if (last_slash == path) {
    return "/";
  } else if (last_slash == nullptr) {
    return ".";
  } else {
    return std::string(path, last_slash - path);
  }
}

bool normalize_path(const char* path, std::string* normalized_path) {
  // Input should be an absolute path
  if (path[0] != '/') {
    PRINT("normalize_path - invalid input: \"%s\", the input path should be absolute", path);
    return false;
  }

  const size_t len = strlen(path) + 1;
  char buf[len];

  const char* in_ptr = path;
  char* out_ptr = buf;

  while (*in_ptr != 0) {
    if (*in_ptr == '/') {
      char c1 = in_ptr[1];
      if (c1 == '.') {
        char c2 = in_ptr[2];
        if (c2 == '/') {
          in_ptr += 2;
          continue;
        } else if (c2 == '.' && (in_ptr[3] == '/' || in_ptr[3] == 0)) {
          in_ptr += 3;
          while (out_ptr > buf && *--out_ptr != '/') {
          }
          if (in_ptr[0] == 0) {
            // retain '/'
            out_ptr++;
          }
          continue;
        }
      } else if (c1 == '/') {
        ++in_ptr;
        continue;
      }
    }
    *out_ptr++ = *in_ptr++;
  }

  *out_ptr = 0;
  *normalized_path = buf;
  return true;
}

bool file_is_in_dir(const std::string& file, const std::string& dir) {
  const char* needle = dir.c_str();
  const char* haystack = file.c_str();
  size_t needle_len = strlen(needle);

  return strncmp(haystack, needle, needle_len) == 0 &&
         haystack[needle_len] == '/' &&
         strchr(haystack + needle_len + 1, '/') == nullptr;
}

bool file_is_under_dir(const std::string& file, const std::string& dir) {
  const char* needle = dir.c_str();
  const char* haystack = file.c_str();
  size_t needle_len = strlen(needle);

  return strncmp(haystack, needle, needle_len) == 0 &&
         haystack[needle_len] == '/';
}

const char* const kZipFileSeparator = "!/";

bool parse_zip_path(const char* input_path, std::string* zip_path, std::string* entry_path) {
  std::string normalized_path;
  if (!normalize_path(input_path, &normalized_path)) {
    return false;
  }

  const char* const path = normalized_path.c_str();
  TRACE("Trying zip file open from path \"%s\" -> normalized \"%s\"", input_path, path);

  // Treat an '!/' separator inside a path as the separator between the name
  // of the zip file on disk and the subdirectory to search within it.
  // For example, if path is "foo.zip!/bar/bas/x.so", then we search for
  // "bar/bas/x.so" within "foo.zip".
  const char* const separator = strstr(path, kZipFileSeparator);
  if (separator == nullptr) {
    return false;
  }

  char buf[512];
  if (strlcpy(buf, path, sizeof(buf)) >= sizeof(buf)) {
    PRINT("Warning: ignoring very long library path: %s", path);
    return false;
  }

  buf[separator - path] = '\0';

  *zip_path = buf;
  *entry_path = &buf[separator - path + 2];

  return true;
}

constexpr off64_t kPageMask = ~static_cast<off64_t>(PAGE_SIZE-1);

off64_t page_start(off64_t offset) {
  return offset & kPageMask;
}

bool safe_add(off64_t* out, off64_t a, size_t b) {
  CHECK(a >= 0);
  if (static_cast<uint64_t>(INT64_MAX - a) < b) {
    return false;
  }

  *out = a + b;
  return true;
}

size_t page_offset(off64_t offset) {
  return static_cast<size_t>(offset & (PAGE_SIZE-1));
}

void split_path(const char* path, const char* delimiters,
                std::vector<std::string>* paths) {
  if (path != nullptr && path[0] != 0) {
    *paths = android::base::Split(path, delimiters);
  }
}

void resolve_paths(std::vector<std::string>& paths,
                   std::vector<std::string>* resolved_paths) {
  resolved_paths->clear();
  for (const auto& path : paths) {
    char resolved_path[PATH_MAX];
    const char* original_path = path.c_str();
    if (realpath(original_path, resolved_path) != nullptr) {
      struct stat s;
      if (stat(resolved_path, &s) == 0) {
        if (S_ISDIR(s.st_mode)) {
          resolved_paths->push_back(resolved_path);
        } else {
          DL_WARN("Warning: \"%s\" is not a directory (excluding from path)", resolved_path);
          continue;
        }
      } else {
        DL_WARN("Warning: cannot stat file \"%s\": %s", resolved_path, strerror(errno));
        continue;
      }
    } else {
      std::string zip_path;
      std::string entry_path;

      std::string normalized_path;

      if (!normalize_path(original_path, &normalized_path)) {
        DL_WARN("Warning: unable to normalize \"%s\"", original_path);
        continue;
      }

      if (parse_zip_path(normalized_path.c_str(), &zip_path, &entry_path)) {
        if (realpath(zip_path.c_str(), resolved_path) == nullptr) {
          DL_WARN("Warning: unable to resolve \"%s\": %s", zip_path.c_str(), strerror(errno));
          continue;
        }

        resolved_paths->push_back(std::string(resolved_path) + kZipFileSeparator + entry_path);
      }
    }
  }
}

