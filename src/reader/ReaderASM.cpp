/* Implementation of ReaderASM class for reading Oracle redo logs from ASM
   Copyright (C) 2018-2026 Adam Leszczynski (aleszczynski@bersler.com)

This file is part of OpenLogReplicator.

OpenLogReplicator is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as published
by the Free Software Foundation; either version 3, or (at your option)
any later version.

OpenLogReplicator is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenLogReplicator; see the file LICENSE;  If not see
<http://www.gnu.org/licenses/>.  */

#ifdef LINK_LIBRARY_OCI

#include <algorithm>
#include <cstring>
#include <sstream>

#include "../common/Clock.h"
#include "../common/Ctx.h"
#include "../replicator/DatabaseConnection.h"
#include "../replicator/DatabaseEnvironment.h"
#include "../replicator/DatabaseStatement.h"
#include "ReaderASM.h"

namespace OpenLogReplicator {

    bool ReaderASM::asmGetFileAttributes(const std::string& filePath, uint64_t& fileSizeBlocks,
                                         uint& blockSizeOut) {
        if (!asmConn->connected) {
            ctx->error(10105, "not connected to ASM");
            return false;
        }

        try {
            DatabaseStatement stmt(asmConn);
            stmt.createStatement("DECLARE v_ftype VARCHAR2(100); "
                                 "BEGIN DBMS_DISKGROUP.GETFILEATTR(:1, v_ftype, :2, :3); END;");

            stmt.bindString(1, filePath);

            sb4 fsizeOut = 0, blocksizeOut = 0;
            stmt.bindInt(2, fsizeOut);
            stmt.bindInt(3, blocksizeOut);

            stmt.executeQuery();

            fileSizeBlocks = static_cast<uint64_t>(fsizeOut);
            blockSizeOut = static_cast<uint>(blocksizeOut);

            if (unlikely(ctx->isTraceSet(Ctx::TRACE::DISK)))
                ctx->logTrace(Ctx::TRACE::DISK, "ASM file attributes: " + filePath + " - size=" + std::to_string(fileSizeBlocks) +
                              " blocks, blockSize=" + std::to_string(blockSizeOut) + " bytes");
            return true;
        } catch (const std::exception& e) {
            ctx->error(10105, "ASM GETFILEATTR failed for '" + filePath + "': " + e.what());
            return false;
        }
    }

    bool ReaderASM::asmOpenFile(const std::string& filePath, int& handle,
                                uint64_t& maxBlocks, uint& lblkSize) {
        if (!asmConn->connected) {
            ctx->error(10106, "not connected to ASM");
            return false;
        }

        try {
            DatabaseStatement stmt(asmConn);
            stmt.createStatement("DECLARE "
                                 "  v_path VARCHAR2(4000) := :1; "
                                 "  v_ftype VARCHAR2(100); "
                                 "  v_lblksize NUMBER; "
                                 "  v_filesize NUMBER; "
                                 "  v_pblksize NUMBER; "
                                 "BEGIN "
                                 "  DBMS_DISKGROUP.GETFILEATTR(v_path, v_ftype, v_filesize, v_lblksize); "
                                 "  DBMS_DISKGROUP.OPEN(v_path, 'r', v_ftype, v_lblksize, :2, v_pblksize, v_filesize); "
                                 "  :3 := v_filesize; "
                                 "  :4 := v_lblksize; "
                                 "END;");

            stmt.bindString(1, filePath);

            sb4 handleOut = 0, maxblknumOut = 0, lblksizeOut = 0;
            stmt.bindInt(2, handleOut);
            stmt.bindInt(3, maxblknumOut);
            stmt.bindInt(4, lblksizeOut);

            stmt.executeQuery();

            handle = handleOut;
            maxBlocks = static_cast<uint64_t>(maxblknumOut);
            lblkSize = static_cast<uint>(lblksizeOut);

            if (unlikely(ctx->isTraceSet(Ctx::TRACE::DISK)))
                ctx->logTrace(Ctx::TRACE::DISK, "ASM open: " + filePath + " - handle=" + std::to_string(handle) +
                              ", maxBlocks=" + std::to_string(maxBlocks) + ", blockSize=" + std::to_string(lblkSize));
            return true;
        } catch (const std::exception& e) {
            ctx->error(10106, "ASM OPEN failed for '" + filePath + "': " + e.what());
            return false;
        }
    }

    bool ReaderASM::asmReadBlock(int handle, uint64_t offset, uint numBytes,
                                 uint8_t* buffer, uint bufferSize) {
        if (!asmConn->connected) {
            ctx->error(10107, "not connected to ASM");
            return false;
        }

        if (numBytes > bufferSize) {
            ctx->error(10108, "buffer too small for read: need " + std::to_string(numBytes) +
                       " bytes, have " + std::to_string(bufferSize));
            return false;
        }

        try {
            DatabaseStatement stmt(asmConn);
            stmt.createStatement("BEGIN DBMS_DISKGROUP.READ(:1, :2, :3, :4); END;");

            sb4 handleIn = handle;
            // ASM uses 1-based block indexing
            sb4 offsetIn = static_cast<sb4>(offset + 1);
            sb4 numBytesIn = static_cast<sb4>(numBytes);

            if (unlikely(ctx->isTraceSet(Ctx::TRACE::DISK)))
                ctx->logTrace(Ctx::TRACE::DISK, "DBMS_DISKGROUP.READ: handle=" + std::to_string(handleIn) +
                              ", offset=" + std::to_string(offsetIn) +
                              ", numBytes=" + std::to_string(numBytesIn));

            stmt.bindInt(1, handleIn);
            stmt.bindInt(2, offsetIn);
            stmt.bindInt(3, numBytesIn);
            stmt.bindBinary(4, buffer, bufferSize);

            stmt.executeQuery();
            return true;
        } catch (const std::exception& e) {
            ctx->error(10107, "ASM READ failed at offset " + std::to_string(offset) + ": " + e.what());
            return false;
        }
    }

    bool ReaderASM::asmReadBlocksBatch(int handle, uint64_t startOffset, uint numBlocks,
                                       uint8_t* buffer, uint bufferSize, uint blockSizeVal) {
        if (!asmConn->connected) {
            ctx->error(10107, "not connected to ASM");
            return false;
        }

        if (numBlocks > MAX_BATCH_SIZE) {
            ctx->warning(10119, "batch size " + std::to_string(numBlocks) +
                         " exceeds maximum " + std::to_string(MAX_BATCH_SIZE) + ", limiting");
            numBlocks = MAX_BATCH_SIZE;
        }

        if (numBlocks * blockSizeVal > bufferSize) {
            ctx->error(10108, "buffer too small for batch read: need " +
                       std::to_string(numBlocks * blockSizeVal) + " bytes, have " +
                       std::to_string(bufferSize));
            return false;
        }

        // For small batches, fall back to individual reads
        if (numBlocks <= 2) {
            for (uint i = 0; i < numBlocks; ++i) {
                if (!asmReadBlock(handle, startOffset + i, blockSizeVal,
                                  buffer + (i * blockSizeVal), blockSizeVal))
                    return false;
            }
            return true;
        }

        try {
            // Build PL/SQL block with VARRAY for batch reading
            std::ostringstream sqlStream;
            sqlStream << "DECLARE "
                      << "  TYPE CHARARR IS varray(" << MAX_BATCH_SIZE << ") OF raw(" << blockSizeVal << "); "
                      << "  varBlock CHARARR := CHARARR(); "
                      << "  rcounts NUMBER := 1; "
                      << "  myoffset NUMBER := :1; "
                      << "BEGIN "
                      << "  LOOP "
                      << "    varBlock.extend(); "
                      << "    DBMS_DISKGROUP.READ(:2, myoffset, :3, varBlock(rcounts)); "
                      << "    rcounts := rcounts + 1; "
                      << "    myoffset := myoffset + 1; "
                      << "    EXIT WHEN rcounts > :4 OR rcounts > " << MAX_BATCH_SIZE << "; "
                      << "  END LOOP; ";

            for (uint i = 1; i <= numBlocks && i <= MAX_BATCH_SIZE; ++i)
                sqlStream << "  :buf" << i << " := varBlock(" << i << "); ";

            sqlStream << "END;";

            DatabaseStatement stmt(asmConn);
            stmt.createStatement(sqlStream.str());

            // Bind input parameters (positional: 1=startOffset, 2=handle, 3=blockSize, 4=numBlocks)
            sb4 handleIn = handle;
            sb4 startOffsetIn = static_cast<sb4>(startOffset + 1);  // 1-based indexing
            sb4 blockSizeIn = static_cast<sb4>(blockSizeVal);
            sb4 numBlocksIn = static_cast<sb4>(numBlocks);

            uint bindPos = 1;
            stmt.bindInt(bindPos++, startOffsetIn);
            stmt.bindInt(bindPos++, handleIn);
            stmt.bindInt(bindPos++, blockSizeIn);
            stmt.bindInt(bindPos++, numBlocksIn);

            // Bind output buffers for each block
            for (uint i = 0; i < numBlocks && i < MAX_BATCH_SIZE; ++i)
                stmt.bindBinary(bindPos++, buffer + (i * blockSizeVal), blockSizeVal);

            stmt.executeQuery();

            if (unlikely(ctx->isTraceSet(Ctx::TRACE::DISK)))
                ctx->logTrace(Ctx::TRACE::DISK, "ASM batch read " + std::to_string(numBlocks) + " blocks at offset " +
                              std::to_string(startOffset));
            return true;
        } catch (const std::exception& e) {
            ctx->error(10107, "ASM batch READ failed for " + std::to_string(numBlocks) + " blocks: " + e.what());
            return false;
        }
    }

    bool ReaderASM::asmReadMultipleBlocks(int handle, uint64_t startOffset, uint bytesToRead,
                                          uint bufferCount, uint8_t* buffers[], uint bufferSize) {
        if (!asmConn->connected) {
            ctx->error(10107, "not connected to ASM");
            return false;
        }

        if (bufferCount == 0)
            return true;

        if (bytesToRead > bufferSize) {
            ctx->error(10108, "buffer too small for multi-block read: need " + std::to_string(bytesToRead) +
                       " bytes, have " + std::to_string(bufferSize));
            return false;
        }

        // For single buffer, delegate directly to readBlock
        if (bufferCount == 1)
            return asmReadBlock(handle, startOffset, bytesToRead, buffers[0], bufferSize);

        if (bufferCount > MAX_BATCH_SIZE) {
            ctx->warning(10121, "buffer count " + std::to_string(bufferCount) +
                         " exceeds maximum " + std::to_string(MAX_BATCH_SIZE) + ", limiting");
            bufferCount = MAX_BATCH_SIZE;
        }
        
        // Use the logical block size detected from file
        const uint blocksPerChunk = bytesToRead / logicalBlockSize;

        try {
            // Build dynamic PL/SQL block matching exact buffer count
            std::ostringstream sqlStream;
            sqlStream << "DECLARE "
                      << "  TYPE CHARARR IS VARRAY(" << bufferCount << ") OF RAW(" << bytesToRead << "); "
                      << "  varBlock CHARARR := CHARARR(); "
                      << "  rcounts NUMBER := 1; "
                      << "  myoffset NUMBER := :1; "
                      << "BEGIN "
                      << "  LOOP "
                      << "    varBlock.extend(); "
                      << "    DBMS_DISKGROUP.READ(:2, myoffset, :3, varBlock(rcounts)); "
                      << "    rcounts := rcounts + 1; "
                      << "    myoffset := myoffset + " << blocksPerChunk << "; "
                      << "    EXIT WHEN rcounts > :4; "
                      << "  END LOOP; ";

            // Generate output bindings
            for (uint i = 1; i <= bufferCount; ++i)
                sqlStream << "  :buf" << i << " := varBlock(" << i << "); ";

            sqlStream << "END;";

            DatabaseStatement stmt(asmConn);
            stmt.createStatement(sqlStream.str());

            sb4 handleIn = handle;
            sb4 offsetIn = static_cast<sb4>(startOffset + 1);  // ASM 1-based indexing
            sb4 bytesToReadIn = static_cast<sb4>(bytesToRead);
            sb4 bufCountIn = static_cast<sb4>(bufferCount);

            // Bind input parameters (positional: 1=offset, 2=handle, 3=bytestoread, 4=bufcount)
            uint bindPos = 1;
            stmt.bindInt(bindPos++, offsetIn);
            stmt.bindInt(bindPos++, handleIn);
            stmt.bindInt(bindPos++, bytesToReadIn);
            stmt.bindInt(bindPos++, bufCountIn);

            // Bind output buffers directly to the caller's pointers
            // This avoids the intermediate staging buffer allocation and memcpy
            for (uint i = 0; i < bufferCount; ++i) {
                stmt.bindBinary(bindPos++, buffers[i], bytesToRead);
            }

            stmt.executeQuery();

            if (unlikely(ctx->isTraceSet(Ctx::TRACE::DISK)))
                ctx->logTrace(Ctx::TRACE::DISK, "ASM multi-block read " + std::to_string(bufferCount) + " chunks of " +
                              std::to_string(bytesToRead) + " bytes from offset " + std::to_string(startOffset));
            return true;
        } catch (const std::exception& e) {
            ctx->error(10107, "ASM multi-block READ failed for " + std::to_string(bufferCount) + " buffers: " + e.what());
            return false;
        }
    }

    bool ReaderASM::asmCloseFile(int handle) {
        if (!asmConn->connected) {
            ctx->error(10109, "not connected to ASM");
            return false;
        }

        try {
            DatabaseStatement stmt(asmConn);
            stmt.createStatement("BEGIN DBMS_DISKGROUP.CLOSE(:1); END;");

            sb4 handleIn = handle;
            stmt.bindInt(1, handleIn);

            stmt.executeQuery();

            if (unlikely(ctx->isTraceSet(Ctx::TRACE::DISK)))
                ctx->logTrace(Ctx::TRACE::DISK, "ASM close: handle=" + std::to_string(handle));
            return true;
        } catch (const std::exception& e) {
            ctx->error(10109, "ASM CLOSE failed for handle " + std::to_string(handle) + ": " + e.what());
            return false;
        }
    }

    ReaderASM::ReaderASM(Ctx* newCtx, std::string newAlias, std::string newDatabase,
                         int newGroup, bool newConfiguredBlockSum, DatabaseConnection* newAsmConn) :
            Reader(newCtx, std::move(newAlias), std::move(newDatabase), newGroup, newConfiguredBlockSum),
            asmConn(newAsmConn) {
    }

    ReaderASM::~ReaderASM() {
        ReaderASM::redoClose();
    }

    void ReaderASM::redoClose() {
        if (fileHandle != -1 && asmConn != nullptr && asmConn->connected) {
            contextSet(CONTEXT::OS, REASON::OS);
            asmCloseFile(fileHandle);
            contextSet(CONTEXT::CPU);
            fileHandle = -1;
        }
    }

    Reader::REDO_CODE ReaderASM::redoOpen() {
        if (asmConn == nullptr) {
            ctx->error(10110, "ASM connection not initialized");
            return REDO_CODE::ERROR;
        }

        if (!asmConn->connected) {
            ctx->error(10111, "not connected to ASM instance");
            return REDO_CODE::ERROR;
        }

        asmPath = fileName;

        uint64_t fileSizeBlocks = 0;
        uint fileBlockSize = 0;

        contextSet(CONTEXT::OS, REASON::OS);
        bool success = asmGetFileAttributes(asmPath, fileSizeBlocks, fileBlockSize);
        contextSet(CONTEXT::CPU);

        if (!success) {
            ctx->error(10112, "failed to get ASM file attributes for: " + asmPath);
            return REDO_CODE::ERROR;
        }

        fileSize = fileSizeBlocks * fileBlockSize;
        blockSize = fileBlockSize;

        if ((fileSize & (Ctx::MIN_BLOCK_SIZE - 1)) != 0) {
            fileSize &= ~(Ctx::MIN_BLOCK_SIZE - 1);
            ctx->warning(10113, "ASM file: " + asmPath + " size is not a multiplication of " +
                         std::to_string(Ctx::MIN_BLOCK_SIZE) + ", reading only " +
                         std::to_string(fileSize) + " bytes");
        }

        contextSet(CONTEXT::OS, REASON::OS);
        success = asmOpenFile(asmPath, fileHandle, maxBlockNum, logicalBlockSize);
        contextSet(CONTEXT::CPU);

        if (!success || fileHandle == -1) {
            ctx->error(10114, "failed to open ASM file: " + asmPath);
            return REDO_CODE::ERROR;
        }

        ctx->info(0, "Successfully opened ASM redo log: " + asmPath +
                  " (handle=" + std::to_string(fileHandle) +
                  ", size=" + std::to_string(fileSize) + " bytes)");

        return REDO_CODE::OK;
    }

    int ReaderASM::redoRead(uint8_t* buf, uint64_t offset, uint size) {
        if (fileHandle == -1) {
            ctx->error(10115, "ASM file not open");
            return -1;
        }

        if (asmConn == nullptr || !asmConn->connected) {
            ctx->error(10116, "ASM connection lost");
            return -1;
        }

        uint64_t startTime = 0;
        if (unlikely(ctx->isTraceSet(Ctx::TRACE::PERFORMANCE)))
            startTime = ctx->clock->getTimeUt();

        // Calculate block offset (ASM reads by block number, not byte offset)
        uint64_t blockOffset = offset / logicalBlockSize;
        uint totalBytesRead = 0;

        contextSet(CONTEXT::OS, REASON::OS);

        // Handle Block 0 (File Header) synthesis if reading from start
        if (blockOffset == 0) {
            // Synthesize file header block - ASM uses 1-based indexing, block 0 doesn't exist
            std::memset(buf, 0, logicalBlockSize);

            buf[1] = (logicalBlockSize == 4096) ? 0x82 : 0x22;

            buf[20] = logicalBlockSize & 0xFF;
            buf[21] = (logicalBlockSize >> 8) & 0xFF;
            buf[22] = (logicalBlockSize >> 16) & 0xFF;
            buf[23] = (logicalBlockSize >> 24) & 0xFF;

            buf[28] = 0x7D;
            buf[29] = 0x7C;
            buf[30] = 0x7B;
            buf[31] = 0x7A;

            totalBytesRead = logicalBlockSize;
            blockOffset = 1;  // First real ASM block
        }

        // Optimal chunk size for batch reads: largest multiple of blockSize
        // fitting in Oracle's 32K RAW limit (32256 = 63 * 512)
        const uint blocksPerChunk = 32256 / logicalBlockSize;
        const uint chunkSize = blocksPerChunk * logicalBlockSize;

        // Phase 1: Bulk reads using readMultipleBlocks
        // Each PL/SQL round-trip reads up to 50 chunks of chunkSize bytes,
        // reducing ~200 round-trips/MB to ~1-3 round-trips/MB
        while (totalBytesRead + chunkSize <= size) {
            if (ctx->hardShutdown) {
                contextSet(CONTEXT::CPU);
                return -1;
            }

            uint remainingBytes = size - totalBytesRead;
            uint chunksAvailable = remainingBytes / chunkSize;
            uint batchCount = std::min(chunksAvailable, MAX_BATCH_SIZE);

            // Build pointer array into the contiguous output buffer
            uint8_t* chunkPtrs[MAX_BATCH_SIZE];
            for (uint c = 0; c < batchCount; ++c)
                chunkPtrs[c] = buf + totalBytesRead + (c * chunkSize);

            // asmReadMultipleBlocks takes 0-based offset (adds +1 internally for ASM 1-based indexing)
            bool success = asmReadMultipleBlocks(fileHandle, blockOffset - 1,
                                                 chunkSize, batchCount, chunkPtrs, chunkSize);
            if (!success) {
                contextSet(CONTEXT::CPU);
                ctx->error(10117, "failed to batch read from ASM at block " +
                          std::to_string(blockOffset));
                return -1;
            }

            totalBytesRead += batchCount * chunkSize;
            blockOffset += batchCount * blocksPerChunk;
        }

        // Phase 2: Read remaining blocks (< chunkSize) using readBlocksBatch
        uint remainingBlocks = (size - totalBytesRead) / logicalBlockSize;
        if (remainingBlocks > 0) {
            while (remainingBlocks > 0) {
                if (ctx->hardShutdown) {
                    contextSet(CONTEXT::CPU);
                    return -1;
                }

                uint batchSize = std::min(remainingBlocks, MAX_BATCH_SIZE);
                uint batchBytes = batchSize * logicalBlockSize;

                bool success = asmReadBlocksBatch(fileHandle, blockOffset - 1,
                                                  batchSize, buf + totalBytesRead,
                                                  batchBytes, logicalBlockSize);
                if (!success) {
                    contextSet(CONTEXT::CPU);
                    ctx->error(10117, "failed to read remainder from ASM at block " +
                              std::to_string(blockOffset));
                    return -1;
                }

                totalBytesRead += batchBytes;
                blockOffset += batchSize;
                remainingBlocks -= batchSize;
            }
        }

        contextSet(CONTEXT::CPU);

        if (unlikely(ctx->isTraceSet(Ctx::TRACE::PERFORMANCE))) {
            sumRead += size;
            sumTime += ctx->clock->getTimeUt() - startTime;
        }

        return static_cast<int>(size);
    }

    void ReaderASM::showHint(Thread* t, std::string origPath, std::string mappedPath) const {
        ctx->hint("trying to read ASM file: " + origPath);

        if (!mappedPath.empty() && mappedPath != origPath)
            ctx->hint("ASM path mapped to: " + mappedPath);

        ctx->hint("verify ASM connection string, credentials, and file path");
        ctx->hint("ensure file exists in ASM: SQL> SELECT name FROM v$archived_log WHERE name LIKE '%" + origPath + "%';");
        ctx->hint("verify ASM disk group is mounted: SQL> SELECT name, state FROM v$asm_diskgroup;");
    }
}

#endif // LINK_LIBRARY_OCI
