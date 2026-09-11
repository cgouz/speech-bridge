#include "multipart.h"

#include <Multipart.h>
#include <stdexcept>

namespace sb::httpapi {

namespace {

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

}  // namespace

ParsedMultipart ParseMultipart(const std::string &contentType, std::string &body) {
  uWS::MultipartParser parser(contentType);
  if (!parser.isValid()) throw std::runtime_error("multipart: no boundary in Content-Type");

  // Multipart.h pokes one sentinel byte just past each part it scans; the
  // view must stay inside a buffer with spare writable capacity.
  if (body.capacity() <= body.size()) body.reserve(body.size() + 1);
  parser.setBody(std::string_view(body.data(), body.size()));

  ParsedMultipart out;
  std::pair<std::string_view, std::string_view> headers[MAX_HEADERS];
  std::optional<std::string_view> part;
  while ((part = parser.getNextPart(headers)).has_value()) {
    std::string_view name, filename;
    bool isFormData = false;
    for (auto &h : headers) {
      if (h.first.empty()) break;
      if (h.first == "content-disposition") {
        uWS::ParameterParser pp(h.second);
        for (;;) {
          auto [key, value] = pp.getKeyValue();
          if (key.empty()) break;
          if (key == "form-data") isFormData = true;
          else if (key == "name") name = value;
          else if (key == "filename") filename = value;
        }
      }
    }
    if (!isFormData || name.empty()) continue;

    if (!filename.empty()) {
      out.file = MultipartFile{std::string(name), std::string(filename), std::string(*part)};
    } else {
      out.fields[std::string(name)] = std::string(Trim(*part));
    }
  }
  return out;
}

}  // namespace sb::httpapi
