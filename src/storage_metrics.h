/*
 * s3fs - FUSE-based file system backed by Amazon S3
 *
 * Copyright(C) 2007 Randy Rizun <rrizun@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef S3FS_STORAGE_METRICS_H_
#define S3FS_STORAGE_METRICS_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

//------------------------------------------------
// Class StorageMetrics
//------------------------------------------------
// Tracks used storage capacity of the S3 bucket via:
//   1) Background thread that enumerates all objects using ListObjectsV2
//   2) Local incremental bookkeeping for write/delete operations
//   3) Periodic re-scan to correct drift from external changes
//
class StorageMetrics
{
    private:
        static std::unique_ptr<StorageMetrics> singleton;

        // Total used bytes (from scan + incremental adjustments)
        std::atomic<int64_t>  used_bytes{0};

        // Whether the initial scan has completed
        std::atomic<bool>     scan_completed{false};

        // Whether the metrics feature is enabled
        bool                  enabled{false};

        // Re-scan interval in seconds (default 6 hours)
        int                   rescan_interval_sec{21600};

        // Background scan thread
        std::thread           scan_thread;
        std::atomic<bool>     exit_flag{false};
        std::mutex            scan_mutex;

        // Internal: perform a full bucket scan via ListObjectsV2
        int64_t ScanBucketSize();

        // Background thread worker
        void ScanWorker();

    public:
        StorageMetrics() = default;
        ~StorageMetrics();
        StorageMetrics(const StorageMetrics&) = delete;
        StorageMetrics& operator=(const StorageMetrics&) = delete;

        // Initialize / Destroy the singleton
        static bool Initialize(bool enable, int rescan_interval = 21600);
        static void Destroy();

        // Check if metrics tracking is enabled and scan is complete
        static bool IsEnabled();
        static bool IsScanComplete();

        // Get the current used bytes
        static int64_t GetUsedBytes();

        // Incremental bookkeeping: call these when objects change
        static void AddBytes(int64_t bytes);
        static void SubBytes(int64_t bytes);
};

#endif // S3FS_STORAGE_METRICS_H_

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
