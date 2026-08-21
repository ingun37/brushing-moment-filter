#pragma once

#include <QString>
#include <expected>

// MD5 of the whole file contents, hashed with FFmpeg's libavutil/md5.h.
// Returns the lowercase hex digest, or an error message.
std::expected<QString, QString> computeFileMd5(const QString &path);
