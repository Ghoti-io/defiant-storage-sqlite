/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Defiant Storage SQLite.
 *
 * Ghoti.io Defiant Storage SQLite is free software: you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public License
 * version 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Defiant Storage SQLite is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * A fresh directory under the system temporary directory. mkdtemp is POSIX
 * and a Windows build has none, so the tests share this one.
 */

#ifndef GDEFSSQLITE_TESTS_TMPDIR_H
#define GDEFSSQLITE_TESTS_TMPDIR_H

#include <filesystem>
#include <random>
#include <string>
#include <system_error>

/** The path of a new empty directory the owner alone can use, or "". */
inline std::string MakeTempDir(const char * stem) {
  std::random_device entropy;
  std::error_code ec;
  std::filesystem::path base = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return std::string();
  }
  for (int attempt = 0; attempt < 100; attempt++) {
    std::filesystem::path path = base / (std::string(stem) + std::to_string(entropy()));
    if (std::filesystem::create_directory(path, ec) && !ec) {
      std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
      return path.string();
    }
  }
  return std::string();
}

#endif /* GDEFSSQLITE_TESTS_TMPDIR_H */
