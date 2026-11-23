// SPDX-License-Identifier: Apache-2.0
//
// microStore filesystem adapter for LittleFS in the arduino-pico core.
//
// microStore's bundled LittleFSFileSystem adapter targets the ESP32
// (LittleFS.begin(formatOnFail, basepath), totalBytes()). arduino-pico has the
// ESP8266-style FS API instead (begin(), info(), stat(), Dir).
//
// microReticulum uses relative paths like "./cache" or "./foo"; they are
// mapped to absolute LittleFS paths ("/cache", "/foo") here.
#pragma once

#include <Arduino.h>
#include <LittleFS.h>

#include <microStore/File.h>
#include <microStore/FileSystem.h>

#include <memory>
#include <string>

namespace madpico {

inline std::string normalize_path(const char* path) {
	std::string p = path ? path : "";
	while (p.rfind("./", 0) == 0) p.erase(0, 2);
	if (p == ".") p.clear();
	if (p.empty() || p[0] != '/') p.insert(0, "/");
	while (p.size() > 1 && p.back() == '/') p.pop_back();
	return p;
}

class PicoLittleFS : public microStore::FileSystem {

public:
	PicoLittleFS() : microStore::FileSystem(new FileSystemImpl()) {}

protected:

	class FileImpl : public microStore::FileImpl {
	private:
		fs::File _file;
		std::string _name;
		bool _closed = false;

	public:
		FileImpl(fs::File file) : microStore::FileImpl(), _file(file), _name(file.name()) {}
		virtual ~FileImpl() { if (!_closed) close(); }

		const char* name() const override { return _name.c_str(); }
		size_t size() const override { return const_cast<fs::File&>(_file).size(); }
		void close() override { _file.close(); _closed = true; }

		int read() override { return _file.read(); }
		size_t write(uint8_t ch) override { return _file.write(ch); }
		size_t read(uint8_t* buffer, size_t size) override { return _file.read(buffer, size); }
		size_t write(const uint8_t* buffer, size_t size) override { return _file.write(buffer, size); }

		int available() override { return _file.available(); }
		int peek() override { return _file.peek(); }
		size_t tell() override { return _file.position(); }
		long seek(uint32_t pos, microStore::SeekMode mode) override {
			fs::SeekMode smode = fs::SeekSet;
			if (mode == microStore::SeekModeCur) smode = fs::SeekCur;
			else if (mode == microStore::SeekModeEnd) smode = fs::SeekEnd;
			return _file.seek(pos, smode) ? (long)_file.position() : -1;
		}
		void flush() override { _file.flush(); }
		bool isValid() const override { return !_closed && (bool)const_cast<fs::File&>(_file); }
	};

	class FileSystemImpl : public microStore::FileSystemImpl {
	public:
		bool format() override {
			Serial.println("[fs] Formatting LittleFS ...");
			return LittleFS.format();
		}

		bool init(bool reformatOnFail = true) override {
			// arduino-pico formats automatically if mounting fails (autoFormat)
			if (!LittleFS.begin()) {
				Serial.println("[fs] LittleFS.begin() failed");
				if (!reformatOnFail || !format() || !LittleFS.begin()) return false;
			}
			return true;
		}

		microStore::File open(const char* path, microStore::File::Mode mode, const bool create = false) override {
			const char* pmode;
			switch (mode) {
				case microStore::File::ModeRead:       pmode = "r";  break;
				case microStore::File::ModeWrite:      pmode = "w";  break;
				case microStore::File::ModeAppend:     pmode = "a";  break;
				case microStore::File::ModeReadWrite:  pmode = "w+"; break;
				case microStore::File::ModeReadAppend: pmode = "a+"; break;
				default: return {};
			}
			const std::string p = normalize_path(path);
			fs::File f = LittleFS.open(p.c_str(), pmode);
			if (!f) return {};
			return microStore::File(new FileImpl(f));
		}

		bool exists(const char* path) override { return LittleFS.exists(normalize_path(path).c_str()); }
		bool remove(const char* path) override { return LittleFS.remove(normalize_path(path).c_str()); }
		bool rename(const char* from, const char* to) override {
			const std::string t = normalize_path(to);
			// POSIX semantics: overwrite the target
			if (LittleFS.exists(t.c_str())) LittleFS.remove(t.c_str());
			return LittleFS.rename(normalize_path(from).c_str(), t.c_str());
		}
		bool mkdir(const char* path) override { return LittleFS.mkdir(normalize_path(path).c_str()); }
		bool rmdir(const char* path) override { return LittleFS.rmdir(normalize_path(path).c_str()); }

		bool isDirectory(const char* path) override {
			FSStat st;
			if (!LittleFS.stat(normalize_path(path).c_str(), &st)) return false;
			return st.isDir;
		}

		std::list<std::string> listDirectory(const char* path, Callbacks::DirectoryListing callback = nullptr) override {
			std::list<std::string> files;
			fs::Dir dir = LittleFS.openDir(normalize_path(path).c_str());
			while (dir.next()) {
				String name = dir.fileName();
				if (callback) callback(name.c_str());
				else files.push_back(name.c_str());
			}
			return files;
		}

		size_t storageSize() override {
			FSInfo info;
			return LittleFS.info(info) ? (size_t)info.totalBytes : 0;
		}

		size_t storageAvailable() override {
			FSInfo info;
			return LittleFS.info(info) ? (size_t)(info.totalBytes - info.usedBytes) : 0;
		}
	};
};

} // namespace madpico
