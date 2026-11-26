#include "onelake_fs.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

static constexpr const char *ONELAKE_HOST = "onelake.dfs.fabric.microsoft.com";

// The database's virtual file system itself: the one GetFileSystem() wraps takes no opener, and
// httpfs needs the opener to find the http secret.
static FileSystem &Underlying(DatabaseInstance &db) {
	return *DBConfig::GetConfig(db).file_system;
}

string OneLakeHttpFileSystem::HttpsUrl(const string &path) {
	if (!StringUtil::StartsWith(StringUtil::Lower(path), "abfss://")) {
		return "";
	}
	auto at = path.find('@');
	auto slash = path.find('/', 8);
	if (at == string::npos || slash == string::npos || at > slash) {
		return "";
	}
	auto host = path.substr(at + 1, slash - at - 1);
	if (StringUtil::Lower(host) != ONELAKE_HOST) {
		return "";
	}
	return "https://" + host + "/" + path.substr(8, at - 8) + path.substr(slash);
}

bool OneLakeHttpFileSystem::CanHandleFile(const string &path) {
	return !HttpsUrl(path).empty();
}

unique_ptr<FileHandle> OneLakeHttpFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                       optional_ptr<FileOpener> opener) {
	return Underlying(db).OpenFile(HttpsUrl(path), flags, opener);
}

bool OneLakeHttpFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	return Underlying(db).FileExists(HttpsUrl(filename), opener);
}

// A handle opened here is httpfs's: what it does is httpfs's to do.
void OneLakeHttpFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	handle.file_system.Read(handle, buffer, nr_bytes, location);
}

int64_t OneLakeHttpFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	return handle.file_system.Read(handle, buffer, nr_bytes);
}

int64_t OneLakeHttpFileSystem::GetFileSize(FileHandle &handle) {
	return handle.file_system.GetFileSize(handle);
}

timestamp_t OneLakeHttpFileSystem::GetLastModifiedTime(FileHandle &handle) {
	return handle.file_system.GetLastModifiedTime(handle);
}

void OneLakeHttpFileSystem::Seek(FileHandle &handle, idx_t location) {
	handle.file_system.Seek(handle, location);
}

void OneLakeHttpFileSystem::Reset(FileHandle &handle) {
	handle.file_system.Reset(handle);
}

idx_t OneLakeHttpFileSystem::SeekPosition(FileHandle &handle) {
	return handle.file_system.SeekPosition(handle);
}

bool OneLakeHttpFileSystem::CanSeek() {
	return true;
}

bool OneLakeHttpFileSystem::OnDiskFile(FileHandle &handle) {
	return false;
}

std::string OneLakeHttpFileSystem::GetName() const {
	return "OneLakeHttpFileSystem";
}

} // namespace duckdb
