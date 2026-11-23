// SPDX-License-Identifier: Apache-2.0
//
// Simple line-based file transfer protocol over USB serial, used by
// tools/madpico.py to manage the pages stored in LittleFS without touching
// the rest of the filesystem (node identity etc.).
//
// Commands are lines starting with '!' and terminated by '\n'. Every response
// line also starts with '!'; any other output (log lines) must be ignored by
// the host. Paths are relative to the pages directory.
//
//   !ping                       -> !ok madpico <version>
//   !ls                         -> !file <size> <crc32> <path>  (per file)
//                                  !ok <count>
//   !df                         -> !ok <used_bytes> <total_bytes>
//   !put <path> <size> <crc32>  -> !ready, then the host sends exactly <size>
//                                  raw bytes -> !ok | !err <message>
//   !get <path>                 -> !data <size> <crc32>\n<raw bytes>\n!ok
//   !rm <path>                  -> !ok | !err <message>
//
// <crc32> is lowercase hex, identical to Python's zlib.crc32().
#pragma once

#include <Arduino.h>
#include <LittleFS.h>

#include <string>
#include <vector>

#include "Crc32.h"
#include "config.h"

namespace madpico {

class FileTransfer {

public:
	using Callback = void (*)(const std::string& rel_path);

	FileTransfer(const char* root, size_t max_file_size, Callback on_added, Callback on_removed)
		: _root(root), _max_size(max_file_size), _on_added(on_added), _on_removed(on_removed) {}

	// Handles one command line (without the leading '!')
	void handle_line(const std::string& line) {
		std::vector<std::string> args = split(line);
		if (args.empty()) return err("empty command");
		const std::string& cmd = args[0];

		if (cmd == "ping") {
			Serial.printf("!ok madpico %s\n", FW_VERSION);
		}
		else if (cmd == "ls") {
			unsigned count = 0;
			list_dir(_root, "", count);
			Serial.printf("!ok %u\n", count);
		}
		else if (cmd == "df") {
			FSInfo info;
			if (!LittleFS.info(info)) return err("fs info failed");
			Serial.printf("!ok %lu %lu\n", (unsigned long)info.usedBytes, (unsigned long)info.totalBytes);
		}
		else if (cmd == "put" && args.size() == 4) {
			put(args[1], strtoul(args[2].c_str(), nullptr, 10), strtoul(args[3].c_str(), nullptr, 16));
		}
		else if (cmd == "get" && args.size() == 2) {
			get(args[1]);
		}
		else if (cmd == "rm" && args.size() == 2) {
			rm(args[1]);
		}
		else {
			err("unknown command or wrong arguments");
		}
		Serial.flush();
	}

	// Rejects absolute paths, "..", hidden files and unusual characters
	static bool valid_path(const std::string& p) {
		if (p.empty() || p.size() > 80) return false;
		if (p[0] == '/' || p.back() == '/') return false;
		size_t seg_start = 0;
		for (size_t i = 0; i <= p.size(); ++i) {
			if (i == p.size() || p[i] == '/') {
				if (i == seg_start) return false;          // empty segment ("//")
				if (p[seg_start] == '.') return false;     // hidden / "." / ".."
				seg_start = i + 1;
				continue;
			}
			char c = p[i];
			bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
			if (!ok) return false;
		}
		return true;
	}

private:
	static constexpr uint32_t DATA_TIMEOUT_MS = 5000;

	static std::vector<std::string> split(const std::string& s) {
		std::vector<std::string> out;
		size_t i = 0;
		while (i < s.size()) {
			while (i < s.size() && s[i] == ' ') ++i;
			size_t j = i;
			while (j < s.size() && s[j] != ' ') ++j;
			if (j > i) out.push_back(s.substr(i, j - i));
			i = j;
		}
		return out;
	}

	static void err(const char* msg) { Serial.printf("!err %s\n", msg); }

	static uint32_t file_crc(const char* path, size_t& size) {
		fs::File f = LittleFS.open(path, "r");
		size = 0;
		if (!f) return 0;
		uint8_t buf[256];
		uint32_t crc = 0;
		int n;
		while ((n = f.read(buf, sizeof(buf))) > 0) {
			crc = crc32_update(crc, buf, (size_t)n);
			size += (size_t)n;
		}
		f.close();
		return crc;
	}

	void list_dir(const std::string& dir, const std::string& rel, unsigned& count) {
		fs::Dir d = LittleFS.openDir(dir.c_str());
		while (d.next()) {
			std::string name = d.fileName().c_str();
			if (name.empty() || name[0] == '.') continue;  // temp files
			std::string full = dir + "/" + name;
			std::string r = rel.empty() ? name : rel + "/" + name;
			if (d.isDirectory()) {
				list_dir(full, r, count);
				continue;
			}
			size_t size;
			uint32_t crc = file_crc(full.c_str(), size);
			Serial.printf("!file %u %08lx %s\n", (unsigned)size, (unsigned long)crc, r.c_str());
			++count;
		}
	}

	void put(const std::string& rel, size_t size, uint32_t expected_crc) {
		if (!valid_path(rel)) return err("invalid path");
		if (size > _max_size) return err("file too large");
		FSInfo info;
		if (LittleFS.info(info) && info.totalBytes - info.usedBytes < size + 8192) {
			return err("not enough space");
		}

		// Write to a hidden temp file in the target directory, then rename
		const std::string target = _root + "/" + rel;
		const size_t slash = target.rfind('/');
		const std::string tmp = target.substr(0, slash + 1) + ".upload.tmp";
		fs::File f = LittleFS.open(tmp.c_str(), "w");
		if (!f) return err("cannot create file");

		Serial.println("!ready");
		Serial.flush();

		uint8_t buf[512];
		size_t received = 0;
		uint32_t crc = 0;
		uint32_t last = millis();
		bool write_ok = true;
		while (received < size) {
			int avail = Serial.available();
			if (avail <= 0) {
				if (millis() - last > DATA_TIMEOUT_MS) break;
				delay(1);
				continue;
			}
			size_t want = size - received;
			if (want > sizeof(buf)) want = sizeof(buf);
			if (want > (size_t)avail) want = (size_t)avail;
			size_t n = Serial.readBytes(buf, want);
			if (n == 0) continue;
			crc = crc32_update(crc, buf, n);
			if (f.write(buf, n) != n) write_ok = false;
			received += n;
			last = millis();
		}
		f.close();

		if (received != size) {
			LittleFS.remove(tmp.c_str());
			return err("timeout while receiving data");
		}
		if (!write_ok) {
			LittleFS.remove(tmp.c_str());
			return err("write failed");
		}
		if (crc != expected_crc) {
			LittleFS.remove(tmp.c_str());
			return err("crc mismatch");
		}
		if (LittleFS.exists(target.c_str())) LittleFS.remove(target.c_str());
		if (!LittleFS.rename(tmp.c_str(), target.c_str())) {
			LittleFS.remove(tmp.c_str());
			return err("rename failed");
		}
		if (_on_added) _on_added(rel);
		Serial.println("!ok");
	}

	void get(const std::string& rel) {
		if (!valid_path(rel)) return err("invalid path");
		const std::string path = _root + "/" + rel;
		size_t size;
		uint32_t crc = file_crc(path.c_str(), size);
		fs::File f = LittleFS.open(path.c_str(), "r");
		if (!f) return err("not found");
		Serial.printf("!data %u %08lx\n", (unsigned)size, (unsigned long)crc);
		uint8_t buf[256];
		int n;
		while ((n = f.read(buf, sizeof(buf))) > 0) Serial.write(buf, (size_t)n);
		f.close();
		Serial.print("\n!ok\n");
	}

	void rm(const std::string& rel) {
		if (!valid_path(rel)) return err("invalid path");
		const std::string path = _root + "/" + rel;
		if (!LittleFS.exists(path.c_str())) return err("not found");
		if (!LittleFS.remove(path.c_str())) return err("remove failed");
		if (_on_removed) _on_removed(rel);
		Serial.println("!ok");
	}

	std::string _root;
	size_t _max_size;
	Callback _on_added;
	Callback _on_removed;
};

} // namespace madpico
