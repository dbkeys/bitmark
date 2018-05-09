// Copyright (c) 2009-2010 Satoshi Nakamoto
// Original Code: Copyright (c) 2009-2014 The Bitcoin Core Developers
// Modified Code: Copyright (c) 2014 Project Bitmark
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "miner.h"

#include "core.h"
#include "main.h"
#include "net.h"
#ifdef ENABLE_WALLET
#include "wallet.h"
#endif
#include "tromp/equi_miner.h"
#include "equihash.h"

//////////////////////////////////////////////////////////////////////////////
//
// BitmarkMiner
//

int static FormatHashBlocks(void* pbuffer, unsigned int len)
{
    unsigned char* pdata = (unsigned char*)pbuffer;
    unsigned int blocks = 1 + ((len + 8) / 64);
    unsigned char* pend = pdata + 64 * blocks;
    memset(pdata + len, 0, 64 * blocks - len);
    pdata[len] = 0x80;
    unsigned int bits = len * 8;
    pend[-1] = (bits >> 0) & 0xff;
    pend[-2] = (bits >> 8) & 0xff;
    pend[-3] = (bits >> 16) & 0xff;
    pend[-4] = (bits >> 24) & 0xff;
    return blocks;
}

static const unsigned int pSHA256InitState[8] =
{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

void SHA256Transform(void* pstate, void* pinput, const void* pinit)
{
    SHA256_CTX ctx;
    unsigned char data[64];

    SHA256_Init(&ctx);

    for (int i = 0; i < 16; i++)
        ((uint32_t*)data)[i] = ByteReverse(((uint32_t*)pinput)[i]);

    for (int i = 0; i < 8; i++)
        ctx.h[i] = ((uint32_t*)pinit)[i];

    SHA256_Update(&ctx, data, sizeof(data));
    for (int i = 0; i < 8; i++)
        ((uint32_t*)pstate)[i] = ctx.h[i];
}

// Some explaining would be appreciated
class COrphan
{
public:
    const CTransaction* ptx;
    set<uint256> setDependsOn;
    double dPriority;
    double dFeePerKb;

    COrphan(const CTransaction* ptxIn)
    {
        ptx = ptxIn;
        dPriority = dFeePerKb = 0;
    }

    void print() const
    {
        LogPrintf("COrphan(hash=%s, dPriority=%.1f, dFeePerKb=%.1f)\n",
               ptx->GetHash().ToString(), dPriority, dFeePerKb);
        BOOST_FOREACH(uint256 hash, setDependsOn)
            LogPrintf("   setDependsOn %s\n", hash.ToString());
    }
};


uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;

// We want to sort transactions by priority and fee, so:
typedef boost::tuple<double, double, const CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;
public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) { }
    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee)
        {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        }
        else
        {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();
            return a.get<0>() < b.get<0>();
        }
    }
};

CBlockTemplate* CreateNewBlock(const CScript& scriptPubKeyIn)
{
    // Create new block
    auto_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if(!pblocktemplate.get())
        return NULL;
    CBlock *pblock = &pblocktemplate->block; // pointer for convenience

    CBlockIndex* pindexPrev = chainActive.Tip();
    miningAlgo = GetArg("-miningalgo", miningAlgo);
    LogPrintf("pindexPrev nHeight = %d while nForkHeight = %d\n",pindexPrev->nHeight,nForkHeight);
    if (pindexPrev->nHeight >= nForkHeight - 1 && CBlockIndex::IsSuperMajority(4,pindexPrev,75,100)) {
      LogPrintf("algo set to %d\n",miningAlgo);
      //pblock->nVersion = 3;
      LogPrintf("pblock nVersion is %d\n",pblock->nVersion);
      pblock->SetAlgo(miningAlgo);
      LogPrintf("after setting algo to %d, it is %d\n",miningAlgo,pblock->nVersion);
    }

    // Create coinbase tx
    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);
    txNew.vout[0].scriptPubKey = scriptPubKeyIn;

    // Add our coinbase tx as first transaction
    pblock->vtx.push_back(txNew);
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE-1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", DEFAULT_BLOCK_MIN_SIZE);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Collect memory pool transactions into the block
    int64_t nFees = 0;
    {
        LOCK2(cs_main, mempool.cs);
        CBlockIndex* pindexPrev = chainActive.Tip();
        CCoinsViewCache view(*pcoinsTip, true);

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;
        bool fPrintPriority = GetBoolArg("-printpriority", false);

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());
        for (map<uint256, CTxMemPoolEntry>::iterator mi = mempool.mapTx.begin();
             mi != mempool.mapTx.end(); ++mi)
        {
            const CTransaction& tx = mi->second.GetTx();
            if (tx.IsCoinBase() || !IsFinalTx(tx, pindexPrev->nHeight + 1))
                continue;

            COrphan* porphan = NULL;
            double dPriority = 0;
            int64_t nTotalIn = 0;
            bool fMissingInputs = false;
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
            {
                // Read prev transaction
                if (!view.HaveCoins(txin.prevout.hash))
                {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if (!mempool.mapTx.count(txin.prevout.hash))
                    {
                        LogPrintf("ERROR: mempool transaction missing input\n");
                        if (fDebug) assert("mempool transaction missing input" == 0);
                        fMissingInputs = true;
                        if (porphan)
                            vOrphan.pop_back();
                        break;
                    }

                    // Has to wait for dependencies
                    if (!porphan)
                    {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }
                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].GetTx().vout[txin.prevout.n].nValue;
                    continue;
                }
                const CCoins &coins = view.GetCoins(txin.prevout.hash);

                int64_t nValueIn = coins.vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = pindexPrev->nHeight - coins.nHeight + 1;

                dPriority += (double)nValueIn * nConf;
            }
            if (fMissingInputs) continue;

            // Priority is sum(valuein * age) / modified_txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority = tx.ComputePriority(dPriority, nTxSize);

            // This is a more accurate fee-per-kilobyte than is used by the client code, because the
            // client code rounds up the size to the nearest 1K. That's good, because it gives an
            // incentive to create smaller transactions.
            double dFeePerKb =  double(nTotalIn-tx.GetValueOut()) / (double(nTxSize)/1000.0);

            if (porphan)
            {
                porphan->dPriority = dPriority;
                porphan->dFeePerKb = dFeePerKb;
            }
            else
                vecPriority.push_back(TxPriority(dPriority, dFeePerKb, &mi->second.GetTx()));
        }

        // Collect transactions into block
        uint64_t nBlockSize = 1000;
        uint64_t nBlockTx = 0;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty())
        {
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            double dFeePerKb = vecPriority.front().get<1>();
            const CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
                continue;

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = GetLegacySigOpCount(tx);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            // Skip free transactions if we're past the minimum block size:
            if (fSortedByFee && (dFeePerKb < CTransaction::nMinRelayTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
                continue;

            // Prioritize by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || !AllowFree(dPriority)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            if (!view.HaveInputs(tx))
                continue;

            int64_t nTxFees = view.GetValueIn(tx)-tx.GetValueOut();

            nTxSigOps += GetP2SHSigOpCount(tx, view);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            CValidationState state;
            if (!CheckInputs(tx, state, view, true, SCRIPT_VERIFY_P2SH))
                continue;

            CTxUndo txundo;
            uint256 hash = tx.GetHash();
            UpdateCoins(tx, state, view, txundo, pindexPrev->nHeight+1, hash);

            // Added
            pblock->vtx.push_back(tx);
            pblocktemplate->vTxFees.push_back(nTxFees);
            pblocktemplate->vTxSigOps.push_back(nTxSigOps);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fPrintPriority)
            {
                LogPrintf("priority %.1f feeperkb %.1f txid %s\n",
                       dPriority, dFeePerKb, tx.GetHash().ToString());
            }

            // Add transactions that depend on this one to the priority queue
            if (mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->dFeePerKb, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;
        LogPrintf("CreateNewBlock(): total size %u\n", nBlockSize);

	if (pindexPrev->nHeight>=nForkHeight-1 && CBlockIndex::IsSuperMajority(4,pindexPrev,75,100)) {
	  LogPrintf("miner on fork\n");
	  CBlockIndex * pprev_algo = pindexPrev;
	  if (GetAlgo(pprev_algo->nVersion)!=miningAlgo) {
	    pprev_algo = get_pprev_algo(pindexPrev,miningAlgo);
	  }
	  if (!pprev_algo) {
	    LogPrintf("miner set update ssf\n");
	    pblock->SetUpdateSSF();
	  }
	  else {
	    LogPrintf("check for update flag\n");
	    char update = 1;
	    for (int i=0; i<nSSF; i++) {
	      if (update_ssf(pprev_algo->nVersion)) {
		LogPrintf("update ssf set on i=%d ago\n",i);
		if (i!=nSSF-1) {
		  update = 0;
		}
		break;
	      }
	      pprev_algo = get_pprev_algo(pprev_algo,-1);
	      if (!pprev_algo) break;
	    }
	    if (update) pblock->SetUpdateSSF();
	  }
	}

	//pblock->vtx[0].vout[0].nValue = GetBlockValue(pindexPrev, nFees);
	pblocktemplate->vTxFees[0] = -nFees;

	// Fill in header
	pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
	//printf("create new block with hash prev = %s (height %d)\n",pblock->hashPrevBlock.GetHex().c_str(),pindexPrev->nHeight);

	UpdateTime(*pblock, pindexPrev);
	pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, miningAlgo);
	LogPrintf("create block nBits = %s\n",CBigNum().SetCompact(pblock->nBits).getuint256().GetHex().c_str());
	pblock->nNonce         = 0;
	if (miningAlgo==ALGO_EQUIHASH) {
	  pblock->nNonce256.SetNull();
	  pblock->nSolution.clear();
	}
	pblock->vtx[0].vin[0].scriptSig = CScript() << OP_0 << OP_0;
	pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(pblock->vtx[0]);
	
        CBlockIndex indexDummy(*pblock);
        indexDummy.pprev = pindexPrev;
        indexDummy.nHeight = pindexPrev->nHeight + 1;

	LogPrintf("GetBlockValue\n");
	pblock->vtx[0].vout[0].nValue = GetBlockValue(&indexDummy, nFees);
	LogPrintf("new view\n");
        CCoinsViewCache viewNew(*pcoinsTip, true);
	LogPrintf("state\n");
        CValidationState state;	
	
        if (!ConnectBlock(*pblock, state, &indexDummy, viewNew, true))
            throw std::runtime_error("CreateNewBlock() : ConnectBlock failed");
    }

    return pblocktemplate.release();
}

void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    pblock->vtx[0].vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(pblock->vtx[0].vin[0].scriptSig.size() <= 100);

    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}


void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1)
{
    //
    // Pre-build hash buffers
    //
    struct
    {
        struct unnamed2
        {
            int nVersion;
            uint256 hashPrevBlock;
            uint256 hashMerkleRoot;
            unsigned int nTime;
            unsigned int nBits;
            unsigned int nNonce;
        }
        block;
        unsigned char pchPadding0[64];
        uint256 hash1;
        unsigned char pchPadding1[64];
    }
    tmp;
    memset(&tmp, 0, sizeof(tmp));

    tmp.block.nVersion       = pblock->nVersion;
    tmp.block.hashPrevBlock  = pblock->hashPrevBlock;
    tmp.block.hashMerkleRoot = pblock->hashMerkleRoot;
    tmp.block.nTime          = pblock->nTime;
    tmp.block.nBits          = pblock->nBits;
    tmp.block.nNonce         = pblock->nNonce;

    FormatHashBlocks(&tmp.block, sizeof(tmp.block));
    FormatHashBlocks(&tmp.hash1, sizeof(tmp.hash1));

    // Byte swap all the input buffer
    for (unsigned int i = 0; i < sizeof(tmp)/4; i++)
        ((unsigned int*)&tmp)[i] = ByteReverse(((unsigned int*)&tmp)[i]);

    // Precalc the first half of the first hash, which stays constant
    SHA256Transform(pmidstate, &tmp.block, pSHA256InitState);

    memcpy(pdata, &tmp.block, 128);
    memcpy(phash1, &tmp.hash1, 64);
}

#ifdef ENABLE_WALLET
//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//
double dHashesPerSec = 0.0;
int64_t nHPSTimerStart = 0;

CBlockTemplate* CreateNewBlockWithKey(CReserveKey& reservekey)
{
    CPubKey pubkey;
    if (!reservekey.GetReservedKey(pubkey))
        return NULL;

    CScript scriptPubKey = CScript() << pubkey << OP_CHECKSIG;
    return CreateNewBlock(scriptPubKey);
}

bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    uint256 hash = pblock->GetPoWHash(miningAlgo);
    if (pblock->nVersion<=2) hash = pblock->GetPoWHash(ALGO_SCRYPT);
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

    if (hash > hashTarget)
        return false;

    //// debug print
    LogPrintf("BitmarkMiner:\n");
    LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", hash.GetHex(), hashTarget.GetHex());
    pblock->print();
    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue));

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
            return error("BitmarkMiner : generated block is stale");

        // Remove key from key pool
        reservekey.KeepKey();

        // Track how many getdata requests this block gets
        {
            LOCK(wallet.cs_wallet);
            wallet.mapRequestCount[pblock->GetHash()] = 0;
        }

        // Process this block the same as if we had received it from another node
        CValidationState state;
        if (!ProcessBlock(state, NULL, pblock))
            return error("BitmarkMiner : ProcessBlock, block not accepted");
    }

    return true;
}

void static BitmarkMiner(CWallet *pwallet)
{
    LogPrintf("BitmarkMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("bitmark-miner");

    // Each thread has its own key and counter
    CReserveKey reservekey(pwallet);
    unsigned int nExtraNonce = 0;

    int n_blocks_created = 0;

    try { while (true) {
        if (Params().NetworkID() != CChainParams::REGTEST) {
            // Busy-wait for the network to come online so we don't waste time mining
            // on an obsolete chain. In regtest mode we expect to fly solo.
            while (vNodes.empty())
                MilliSleep(1000);
        }

	if (Params().NetworkID() == CChainParams::REGTEST) {
	  if (n_blocks_created>0) return;
	}

        //
        // Create new block
        //
        unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
        CBlockIndex* pindexPrev = chainActive.Tip();

        auto_ptr<CBlockTemplate> pblocktemplate(CreateNewBlockWithKey(reservekey));
	n_blocks_created++;
        if (!pblocktemplate.get())
            return;
        CBlock *pblock = &pblocktemplate->block;
        IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);
	//printf("Running BitmarkMiner with %lu transactions in block (%u bytes)\n", pblock->vtx.size(),
	//::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));

        //
        // Pre-build hash buffers
        //
        char pmidstatebuf[32+16]; char* pmidstate = alignup<16>(pmidstatebuf);
        char pdatabuf[128+16];    char* pdata     = alignup<16>(pdatabuf);
        char phash1buf[64+16];    char* phash1    = alignup<16>(phash1buf);

        FormatHashBuffers(pblock, pmidstate, pdata, phash1);

        unsigned int& nBlockTime = *(unsigned int*)(pdata + 64 + 4);
        unsigned int& nBlockBits = *(unsigned int*)(pdata + 64 + 8);
        unsigned int& nBlockNonce = *(unsigned int*)(pdata + 64 + 12);


        //
        // Search
        //
        int64_t nStart = GetTime();
        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

        while (true)
        {
	  unsigned int nHashesDone = 0;
	  uint256 thash;
	  //char scratchpad[SCRYPT_SCRATCHPAD_SIZE];
	  uint256 best_hash;
	  //LogPrintf("starting best hash: %s\n",best_hash.GetHex().c_str());
	  //LogPrintf("hash target = %s\n",hashTarget.GetHex().c_str());
	  bool first_hash = true;

	  if (miningAlgo==ALGO_EQUIHASH) {
	    LogPrintf("Mining algo equihash\n");
	    unsigned int n = Params().EquihashN();
	    unsigned int k = Params().EquihashK();
	    LogPrintf("equi n k = %d %d\n",n,k);
	    bool cancelSolver = false;
	    crypto_generichash_blake2b_state state;
	    EhInitialiseState(n, k, state);
	    CEquihashInput I{*pblock};
	    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
	    ss << I;
	    LogPrintf("ss (%lu) = ",ss.size());
	    for (int i=0; i<ss.size(); i++) {
	      LogPrintf("%02x",*((unsigned char *)&ss[0]+i));
	    }
	    LogPrintf("\n");
	    crypto_generichash_blake2b_update(&state, (unsigned char*)&ss[0], ss.size());
	    crypto_generichash_blake2b_state curr_state;
	    curr_state = state;
	    unsigned char * nonce256 = pblock->nNonce256.begin();
	    LogPrintf("nonce (%lu) = ",pblock->nNonce256.size());
	    for (int i=0; i<pblock->nNonce256.size(); i++) {
	      LogPrintf("%02x",nonce256[i]);
	    }
	    LogPrintf("\n");
	    
	    crypto_generichash_blake2b_update(&curr_state,
					      pblock->nNonce256.begin(),
					      pblock->nNonce256.size());
	    std::function<bool(std::vector<unsigned char>)> validBlock =
	      [&pblock, &hashTarget, &pwallet, &reservekey, &cs_main, &cancelSolver] (std::vector<unsigned char> soln) {
	      pblock->nSolution = soln;
	      //solutionTargetChecks.increment();

	      LogPrintf("check if valid block\n");
	      
	      if (pblock->GetPoWHash(miningAlgo) > hashTarget) {
		return false;
	      }

	      LogPrintf("passed powhash req\n");

	      SetThreadPriority(THREAD_PRIORITY_NORMAL);
	      LogPrintf("ZcashMiner:\n");
	      LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", pblock->GetHash().GetHex(), hashTarget.GetHex());
	      CValidationState state;
	      if (ProcessBlock(state,NULL,pblock)) {
		//std::lock_guard<std::mutex> lock{m_cs};
		LOCK(cs_main);
		cancelSolver = false;
	      }

	      SetThreadPriority(THREAD_PRIORITY_LOWEST);

	      if (Params().MineBlocksOnDemand()) {
		//ehSolverRuns.increment();
		throw boost::thread_interrupted();
	      }

	      return true;
	    };

	    std::function<bool(EhSolverCancelCheck)> cancelled = [&cs_main, &cancelSolver](EhSolverCancelCheck pos) {
	      LOCK(cs_main);
	      return cancelSolver;
	    };

	    /*
	    try {
	      LogPrintf("try ehoptimisedsolve hashprevblock=%s\n",pblock->hashPrevBlock.GetHex().c_str());
	      bool found = EhOptimisedSolve(n, k, curr_state, validBlock, cancelled);
	      //ehSolverRuns.increment();
	      if (found) {
		LogPrintf("ehsolver found\n");
		break;
	      }
	      LogPrintf("ehsolver not found\n");
	    } catch (EhSolverCancelledException&) {
	      LogPrintf("Equihash solver cancelled\n");
	      LOCK(cs_main);
	      cancelSolver = false;
	      }*/

	    //tromp solver
	    equi eq(1);
	    eq.setstate(&curr_state);
	    eq.digit0(0);
	    eq.xfull = eq.bfull = eq.hfull = 0;
	    eq.showbsizes(0);
	    for (u32 r = 1; r < WK; r++) {
	      (r&1) ? eq.digitodd(r, 0) : eq.digiteven(r, 0);
	      eq.xfull = eq.bfull = eq.hfull = 0;
	      eq.showbsizes(r);
	    }
	    eq.digitK(0);
	    //ehSolverRuns.increment();
	    LogPrintf("PROOFSIZE = %d DIGITBITS = %d\n",PROOFSIZE,DIGITBITS);
	    for (size_t s = 0; s < eq.nsols; s++) {
	      LogPrint("pow", "Checking solution %d\n", s+1);
	      std::vector<eh_index> index_vector(PROOFSIZE);
	      for (size_t i = 0; i < PROOFSIZE; i++) {
		index_vector[i] = eq.sols[s][i];
	      }
	      std::vector<unsigned char> sol_char = GetMinimalFromIndices(index_vector, DIGITBITS);
	      if (validBlock(sol_char)) {
		break;
	      }
	    }
	    
	  }
	  else while(true) {
	    
	    uint256 thash = pblock->GetPoWHash(miningAlgo);
	    if (pblock->nVersion<=3) thash = pblock->GetPoWHash(ALGO_SCRYPT);
	    if (thash < best_hash || first_hash) {
	      first_hash = false;
	      best_hash = thash;
	      LogPrintf("best hash: %s\n",best_hash.GetHex().c_str());
	    }
	  
	    if (thash <= hashTarget)
	      {
		SetThreadPriority(THREAD_PRIORITY_NORMAL);
		CheckWork(pblock, *pwallet, reservekey);
		SetThreadPriority(THREAD_PRIORITY_LOWEST);
		break;
	      }
	    pblock->nNonce += 1;
	    nHashesDone += 1;
	    if ((pblock->nNonce & 0xFF) == 0) {
	      //LogPrintf("break 0xff\n");
	      break;
	    }
	  }

	  LogPrintf("Calc hash per sec\n");
	  
	  // Meter hashes/sec
	  static int64_t nHashCounter;
	  if (nHPSTimerStart == 0)
            {
	      nHPSTimerStart = GetTimeMillis();
	      nHashCounter = 0;
            }
	  else
	    nHashCounter += nHashesDone;
	  if (GetTimeMillis() - nHPSTimerStart > 4000)
            {
	      static CCriticalSection cs;
	      {
		LOCK(cs);
		if (GetTimeMillis() - nHPSTimerStart > 4000)
		  {
		    dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
		    nHPSTimerStart = GetTimeMillis();
		    nHashCounter = 0;
		    static int64_t nLogTime;
		    if (GetTime() - nLogTime > 30 * 60)
		      {
			nLogTime = GetTime();
			if (!RegTest()) LogPrintf("hashmeter %6.0f khash/s\n", dHashesPerSec/1000.0);
		      }
		  }
	      }
            }

	  // Check for stop or if block needs to be rebuilt
	  boost::this_thread::interruption_point();
	  if (vNodes.empty() && Params().NetworkID() != CChainParams::REGTEST)
	    break;
	  if (nBlockNonce >= 0xffff0000)
	    break;
	  if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
	    break;
	  if (pindexPrev != chainActive.Tip())
	    break;

	  if (miningAlgo==ALGO_EQUIHASH) {
	    pblock->nNonce256 = (CBigNum(pblock->nNonce256) + 1).getuint256();
	  }
	  // Update nTime every few seconds
	  UpdateTime(*pblock, pindexPrev);
	  nBlockTime = ByteReverse(pblock->nTime);
	  if (TestNet())
            {
	      // Changing pblock->nTime can change work required on testnet:
	      nBlockBits = ByteReverse(pblock->nBits);
	      hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
            }
        }
      } }
    catch (boost::thread_interrupted)
      {
        throw;
      }
}

void GenerateBitmarks(bool fGenerate, CWallet* pwallet, int nThreads)
{

    static boost::thread_group* minerThreads = NULL;

    if (nThreads < 0) {
        if (Params().NetworkID() == CChainParams::REGTEST)
            nThreads = 1;
        else
            nThreads = boost::thread::hardware_concurrency();
    }

    if (minerThreads != NULL)
    {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++)
        minerThreads->create_thread(boost::bind(&BitmarkMiner, pwallet));
}

#endif

