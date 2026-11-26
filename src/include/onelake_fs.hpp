#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"

namespace duckdb {

//! OneLake's files where there is no azure extension (DuckDB-Wasm): an abfss:// path on OneLake,
//! abfss://<workspace>@onelake.dfs.fabric.microsoft.com/<item>/..., is opened as its https:// URL,
//! https://onelake.dfs.fabric.microsoft.com/<workspace>/<item>/..., through the database's file
//! system (httpfs), which sends the bearer token of an http secret. The file's handle is httpfs's,
//! so reading it is httpfs's too.
class OneLakeHttpFileSystem : public FileSystem {
public:
	explicit OneLakeHttpFileSystem(DatabaseInstance &db) : db(db) {
	}

	//! The https:// URL of a OneLake abfss:// path; empty for any other path.
	static string HttpsUrl(const string &path);

	bool CanHandleFile(const string &path) override;
	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;

	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	int64_t GetFileSize(FileHandle &handle) override;
	timestamp_t GetLastModifiedTime(FileHandle &handle) override;
	void Seek(FileHandle &handle, idx_t location) override;
	void Reset(FileHandle &handle) override;
	idx_t SeekPosition(FileHandle &handle) override;
	bool CanSeek() override;
	bool OnDiskFile(FileHandle &handle) override;
	std::string GetName() const override;

private:
	DatabaseInstance &db;
};

} // namespace duckdb
