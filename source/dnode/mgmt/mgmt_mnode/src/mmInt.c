/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http:www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "mmInt.h"
#include "wal.h"

static bool mmDeployRequired(const SMgmtInputOpt *pInput) {
  if (pInput->pData->dnodeId > 0) return false;
  if (pInput->pData->clusterId > 0) return false;
  if (strcmp(tsLocalEp, tsFirst) != 0) return false;
  return true;
}

static int32_t mmRequire(const SMgmtInputOpt *pInput, bool *required) {
  int32_t   code = 0;
  SMnodeOpt option = {0};
  if ((code = mmReadFile(pInput->path, &option)) != 0) {
    return code;
  }

  if (!option.deploy) {
    *required = mmDeployRequired(pInput);
    if (*required) {
      dInfo("deploy mnode required. dnodeId:%d<=0, clusterId:0x%" PRIx64 "<=0, localEp:%s==firstEp",
            pInput->pData->dnodeId, pInput->pData->clusterId, tsLocalEp);
    }
  } else {
    *required = true;
    dInfo("deploy mnode required. option deploy:%d", option.deploy);
  }

  return code;
}

static void mmBuildOptionForDeploy(SMnodeMgmt *pMgmt, const SMgmtInputOpt *pInput, SMnodeOpt *pOption) {
  pOption->deploy = true;
  pOption->msgCb = pMgmt->msgCb;
  pOption->dnodeId = pMgmt->pData->dnodeId;
  pOption->selfIndex = 0;
  pOption->numOfReplicas = 1;
  pOption->numOfTotalReplicas = 1;
  pOption->replicas[0].id = 1;
  pOption->replicas[0].port = tsServerPort;
  tstrncpy(pOption->replicas[0].fqdn, tsLocalFqdn, TSDB_FQDN_LEN);
  pOption->lastIndex = SYNC_INDEX_INVALID;
}

static void mmBuildOptionForOpen(SMnodeMgmt *pMgmt, SMnodeOpt *pOption) {
  pOption->deploy = false;
  pOption->msgCb = pMgmt->msgCb;
  pOption->dnodeId = pMgmt->pData->dnodeId;
}

static void mmClose(SMnodeMgmt *pMgmt) {
  if (pMgmt->pMnode != NULL) {
    mmStopWorker(pMgmt);
    mndClose(pMgmt->pMnode);
    (void)taosThreadRwlockDestroy(&pMgmt->lock);
    pMgmt->pMnode = NULL;
  }

  taosMemoryFree(pMgmt);
}
static int32_t mndOpenWrapper(const char *path, SMnodeOpt *opt, SMnode **pMnode) {
  int32_t code = 0;
  *pMnode = mndOpen(path, opt);
  if (*pMnode == NULL) {
    code = terrno;
  }
  ///*pMnode = pNode;
  return code;
}
static int32_t mmOpen(SMgmtInputOpt *pInput, SMgmtOutputOpt *pOutput) {
  int32_t code = 0;
  if ((code = walInit(pInput->stopDnodeFp)) != 0) {
    dError("failed to init wal since %s", tstrerror(code));
    return code;
  }

  if ((code = syncInit()) != 0) {
    dError("failed to init sync since %s", tstrerror(code));
    return code;
  }

  SMnodeMgmt *pMgmt = taosMemoryCalloc(1, sizeof(SMnodeMgmt));
  if (pMgmt == NULL) {
    code = terrno;
    return code;
  }

  pMgmt->pData = pInput->pData;
  pMgmt->path = pInput->path;
  pMgmt->name = pInput->name;
  pMgmt->msgCb = pInput->msgCb;
  pMgmt->msgCb.putToQueueFp = (PutToQueueFp)mmPutMsgToQueue;
  pMgmt->msgCb.mgmt = pMgmt;
  (void)taosThreadRwlockInit(&pMgmt->lock, NULL);

  SMnodeOpt option = {0};
  if ((code = mmReadFile(pMgmt->path, &option)) != 0) {
    dError("failed to read file since %s", tstrerror(code));
    mmClose(pMgmt);
    return code;
  }

  if (!option.deploy) {
    dInfo("mnode start to deploy");
    pMgmt->pData->dnodeId = 1;
    mmBuildOptionForDeploy(pMgmt, pInput, &option);
  } else {
    dInfo("mnode start to open");
    mmBuildOptionForOpen(pMgmt, &option);
  }

  code = mndOpenWrapper(pMgmt->path, &option, &pMgmt->pMnode);
  if (code != 0) {
    dError("failed to open mnode since %s", tstrerror(code));
    mmClose(pMgmt);
    return code;
  }
  tmsgReportStartup("mnode-impl", "initialized");

  if ((code = mmStartWorker(pMgmt)) != 0) {
    dError("failed to start mnode worker since %s", tstrerror(code));
    mmClose(pMgmt);
    return code;
  }
  tmsgReportStartup("mnode-worker", "initialized");

  if (option.numOfTotalReplicas > 0) {
    option.deploy = true;
    option.numOfReplicas = 0;
    option.numOfTotalReplicas = 0;
    if ((code = mmWriteFile(pMgmt->path, &option)) != 0) {
      dError("failed to write mnode file since %s", tstrerror(code));
      return code;
    }
  }

  pInput->pData->dnodeId = pMgmt->pData->dnodeId;
  pOutput->pMgmt = pMgmt;
  return 0;
}

static int32_t mmStart(SMnodeMgmt *pMgmt) {
  dDebug("mnode-mgmt start to run");
  return mndStart(pMgmt->pMnode);
}

static void mmStop(SMnodeMgmt *pMgmt) {
  dDebug("mnode-mgmt start to stop");
  mndPreClose(pMgmt->pMnode);
  (void)taosThreadRwlockWrlock(&pMgmt->lock);
  pMgmt->stopped = 1;
  (void)taosThreadRwlockUnlock(&pMgmt->lock);

  mndStop(pMgmt->pMnode);
}

static int32_t mmSyncIsCatchUp(SMnodeMgmt *pMgmt) { return mndIsCatchUp(pMgmt->pMnode); }

static ESyncRole mmSyncGetRole(SMnodeMgmt *pMgmt) { return mndGetRole(pMgmt->pMnode); }

SMgmtFunc mmGetMgmtFunc() {
  SMgmtFunc mgmtFunc = {0};
  mgmtFunc.openFp = mmOpen;
  mgmtFunc.closeFp = (NodeCloseFp)mmClose;
  mgmtFunc.startFp = (NodeStartFp)mmStart;
  mgmtFunc.stopFp = (NodeStopFp)mmStop;
  mgmtFunc.createFp = (NodeCreateFp)mmProcessCreateReq;
  mgmtFunc.dropFp = (NodeDropFp)mmProcessDropReq;
  mgmtFunc.requiredFp = mmRequire;
  mgmtFunc.getHandlesFp = mmGetMsgHandles;
  mgmtFunc.isCatchUpFp = (NodeIsCatchUpFp)mmSyncIsCatchUp;
  mgmtFunc.nodeRoleFp = (NodeRole)mmSyncGetRole;

  return mgmtFunc;
}
