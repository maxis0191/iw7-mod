#pragma once

namespace download_cache
{
	// Remembers a file written by the automatic download (mods/<name>/... or usermaps/<name>/...).
	void record_download(const std::string& file);

	// Refreshes the last-used time of a downloaded mod or usermap folder ("mods/<name>" or "usermaps/<name>").
	// Folders without a download record (installed by hand) are ignored.
	void mark_used(const std::string& folder);
}
