/*!
 * $Id$
 *
 * Copyright 2008-2012 Quantcast Corp.
 * Copyright 2006-2008 Kosmix Corp.
 *
 * This file is part of Kosmos File System (KFS).
 *
 * Licensed under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 *
 * \file checkpoint.cc
 * \brief KFS metadata checkpointing
 * \author Sriram Rao and Blake Lewis
 *
 * The metaserver during its normal operation writes out log records.  Every
 * N minutes, the metaserver rolls over the log file.  Periodically, a sequence
 * of log files are compacted to create a checkpoint: a previous checkpoint is
 * loaded and subsequent log files are replayed to update the tree.  At the end
 * of replay, a checkpoint is saved to disk.  To save a checkpoint, we iterate
 * through the leaf nodes of the tree copying the contents of each node to a
 * checkpoint file.
 */

#include "Checkpoint.h"
#include "kfstree.h"
#include "MetaRequest.h"
#include "NetDispatch.h"
#include "util.h"
#include "LayoutManager.h"
#include "common/MdStream.h"
#include "common/FdWriter.h"
#include "common/StBuffer.h"

#include <iostream>
#include <iomanip>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace KFS
{
using std::hex;
using std::dec;

int
Checkpoint::write_leaves(ostream& os)
{
    LeafIter li(metatree.firstLeaf(), 0);
    Meta *m = li.current();
    int status = 0;
    while (status == 0 && m) {
        status = m->checkpoint(os);
        li.next();
        Node* const p = li.parent();
        m = p ? li.current() : 0;
    }
    return status;
}

int
Checkpoint::write(
    const string& logname,
    seq_t         logseq,
    int64_t       errchksum)
{
    if (logname.empty()) {
        return -EINVAL;
    }
    cpname = cpfile(logseq);
    const char* const    suffix  = ".XXXXXX.tmp";
    const size_t         suflen  = strlen(suffix) + 1;
    StBufferT<char, 256> tmpbuf;
    char* const          tmpname = tmpbuf.Reserve(cpname.length() + suflen);
    memcpy(tmpname, cpname.data(), cpname.length());
    memcpy(tmpname + cpname.length(), suffix, suflen);
    int status = 0;
    int fd     = mkstemp(tmpname);
    if (fd < 0) {
        status = errno > 0 ? -errno : -EIO;
    } else {
        close(fd);
        fd = open(tmpname, O_WRONLY | (writesync ? O_SYNC : 0));
        if (fd < 0) {
            status = errno > 0 ? -errno : -EIO;
            unlink(tmpname);
        }
    }
    if (status == 0) {
        FdWriter fdw(fd);
        const bool kSyncFlag = false;
        MdStreamT<FdWriter> os(&fdw, kSyncFlag, string(), writebuffersize);
        os << dec;
        os << "checkpoint/" << logseq << "/" << errchksum << '\n';
        os << "checksum/last-line\n";
        os << "version/" << VERSION << '\n';
        os << "filesysteminfo/fsid/" << metatree.GetFsId() << "/crtime/" <<
            ShowTime(metatree.GetCreateTime()) << '\n';
        os << "fid/" << fileID.getseed() << '\n';
        os << "chunkId/" << chunkID.getseed() << '\n';
        os << "time/" << DisplayIsoDateTime() << '\n';
        os << "setintbase/16\n" << hex;
        os << "log/" << logname << "\n\n";
        status = write_leaves(os);
        if (status == 0 && os) {
            status = gLayoutManager.WritePendingMakeStable(os);
        }
        if (status == 0 && os) {
            status = gLayoutManager.WritePendingChunkVersionChange(os);
        }
        if (status == 0 && os) {
            status = gNetDispatch.WriteCanceledTokens(os);
        }
        if (status == 0 && os) {
            status = gLayoutManager.GetIdempotentRequestTracker().Write(os);
        }
        if (status == 0 && os) {
            status = gLayoutManager.GetUserAndGroup().WriteGroups(os);
        }
        if (status == 0 && os) {
            status = gLayoutManager.WritePendingObjStoreDelete(os);
        }
        if (status == 0) {
            os << "time/" << DisplayIsoDateTime() << '\n';
            const string md = os.GetMd();
            os << "checksum/" << md << '\n';
            os.SetStream(0);
            if ((status = fdw.GetError()) != 0) {
                if (status > 0) {
                    status = -status;
                }
            } else if (! os) {
                status = -EIO;
            }
        }
        if (status == 0) {
            if (close(fd)) {
                status = errno > 0 ? -errno : -EIO;
            } else {
                if (rename(tmpname, cpname.c_str())) {
                    status = errno > 0 ? -errno : -EIO;
                } else {
                    fd = -1;
                    status = link_latest(cpname, LASTCP);
                }
            }
        } else {
            close(fd);
        }
    }
    if (status != 0 && fd >= 0) {
        unlink(tmpname);
    }
    return status;
}

// default values
static string sCPDIR("./kfscp");           //!< directory for CP files
static string sLASTCP(sCPDIR + "/latest"); //!< most recent CP file (link)
Checkpoint cp(CPDIR);

const string& CPDIR  = sCPDIR;
const string& LASTCP = sLASTCP;

void
checkpointer_setup_paths(const string& cpdir)
{
    if (! cpdir.empty()) {
        sCPDIR = cpdir;
        sLASTCP = cpdir + "/latest";
        cp.setCPDir(cpdir);
    }
}

}
