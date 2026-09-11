// multipart — thin wrapper over uWebSockets' bundled Multipart.h parser,
// extracting named form fields and (at most) one file part. Used by
// readAudioRequest (see handlers.cpp), mirroring Go's
// r.ParseMultipartForm/r.FormValue/r.FormFile for our two endpoints' needs.
#pragma once

#include <map>
#include <optional>
#include <string>

namespace sb::httpapi {

struct MultipartFile {
  std::string field_name;  // "file" | "audio"
  std::string filename;
  std::string data;
};

struct ParsedMultipart {
  std::map<std::string, std::string> fields;
  std::optional<MultipartFile> file;
};

// Throws std::runtime_error if contentType has no parseable boundary or the
// body is malformed. `body` must be a mutable buffer (the parser writes one
// sentinel byte just past each part it scans) with capacity() > size(); the
// caller reserves the extra byte.
ParsedMultipart ParseMultipart(const std::string &contentType, std::string &body);

}  // namespace sb::httpapi
