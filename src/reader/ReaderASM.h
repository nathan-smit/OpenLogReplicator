/* Header for ReaderASM class
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

#ifndef READER_ASM_H_
#define READER_ASM_H_

#include "Reader.h"

#ifdef LINK_LIBRARY_OCI

namespace OpenLogReplicator {
    class DatabaseConnection;

    class ReaderASM final : public Reader {
    protected:
        // ASM-specific members
        DatabaseConnection* asmConn{nullptr};
        int fileHandle{-1};
        uint64_t maxBlockNum{0};
        uint logicalBlockSize{0};
        std::string asmPath;

        static constexpr uint MAX_BATCH_SIZE{50};

        // ASM file operations using DBMS_DISKGROUP via DatabaseStatement
        bool asmGetFileAttributes(const std::string& filePath, uint64_t& fileSizeBlocks,
                                  uint& blockSizeOut);
        bool asmOpenFile(const std::string& filePath, int& handle,
                         uint64_t& maxBlocks, uint& lblkSize);
        bool asmReadBlock(int handle, uint64_t offset, uint numBytes,
                          uint8_t* buffer, uint bufferSize);
        bool asmReadBlocksBatch(int handle, uint64_t startOffset, uint numBlocks,
                                uint8_t* buffer, uint bufferSize, uint blockSizeVal);
        bool asmReadMultipleBlocks(int handle, uint64_t startOffset, uint bytesToRead,
                                   uint bufferCount, uint8_t* buffers[], uint bufferSize);
        bool asmCloseFile(int handle);

        // Override Reader virtual methods
        void redoClose() override;
        REDO_CODE redoOpen() override;
        int redoRead(uint8_t* buf, uint64_t offset, uint size) override;

    public:
        ReaderASM(Ctx* newCtx, std::string newAlias, std::string newDatabase,
                  int newGroup, bool newConfiguredBlockSum, DatabaseConnection* newAsmConn);
        ~ReaderASM() override;

        void showHint(Thread* t, std::string origPath, std::string mappedPath) const override;
    };
}

#endif // LINK_LIBRARY_OCI
#endif // READER_ASM_H_
