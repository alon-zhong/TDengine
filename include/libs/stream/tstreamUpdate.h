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
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _TSTREAMUPDATE_H_
#define _TSTREAMUPDATE_H_

#include "taosdef.h"
#include "tarray.h"
#include "tmsg.h"
#include "tscalablebf.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SUpdateInfo {
  SArray *pTsBuckets;
  uint64_t numBuckets;
  SArray *pTsSBFs;
  uint64_t numSBFs;
  int64_t interval;
  int64_t watermark;
  TSKEY minTS;
  SScalableBf* pCloseWinSBF;
  SHashObj* pMap;
  STimeWindow scanWindow;
  uint64_t scanGroupId;
  uint64_t maxVersion;
} SUpdateInfo;

SUpdateInfo *updateInfoInitP(SInterval* pInterval, int64_t watermark);
SUpdateInfo *updateInfoInit(int64_t interval, int32_t precision, int64_t watermark);
bool updateInfoIsUpdated(SUpdateInfo *pInfo, uint64_t tableId, TSKEY ts);
void updateInfoSetScanRange(SUpdateInfo *pInfo, STimeWindow* pWin, uint64_t groupId, uint64_t version);
bool updateInfoIgnore(SUpdateInfo *pInfo, STimeWindow* pWin, uint64_t groupId, uint64_t version);
void updateInfoDestroy(SUpdateInfo *pInfo);
void updateInfoAddCloseWindowSBF(SUpdateInfo *pInfo);
void updateInfoDestoryColseWinSBF(SUpdateInfo *pInfo);
int32_t updateInfoSerialize(void *buf, int32_t bufLen, const SUpdateInfo *pInfo);
int32_t updateInfoDeserialize(void *buf, int32_t bufLen, SUpdateInfo *pInfo);

#ifdef __cplusplus
}
#endif

#endif /* ifndef _TSTREAMUPDATE_H_ */