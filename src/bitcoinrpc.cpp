// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2011-2013 The PPCoin developers
// Copyright (c) 2013-2014 The Slimcoin Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"
#include "wallet.h"
#include "db.h"
#include "walletdb.h"
#include "net.h"
#include "init.h"
#include "checkpoints.h"
#include "ui_interface.h"
#include "bitcoinrpc.h"
#include "kernel.h"
#include "smalldata.h"
#include "util.h"

#undef printf
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/asio/ssl.hpp> 
#include <boost/filesystem/fstream.hpp>
#include <boost/assign/list_of.hpp>
typedef boost::asio::ssl::stream<boost::asio::ip::tcp::socket> SSLStream;

#define printf OutputDebugStringF

#define FORMAT_PARAM(name, arg, type) if (strMethod == name && n > arg) ConvertTo<type> (params[arg])

// MinGW 3.4.5 gets "fatal error: had to relocate PCH" if the json headers are
// precompiled in headers.h.  The problem might be when the pch file goes over
// a certain size around 145MB.  If we need access to json_spirit outside this
// file, we could use the compiled json_spirit option.

// Fix to compile with Boost 1.70+
// https://stackoverflow.com/a/67773642
#if BOOST_VERSION >= 107000
#define GET_IO_SERVICE(s) ((boost::asio::io_context&)(s).get_executor().context())
#else
#define GET_IO_SERVICE(s) ((s).get_io_service())
#endif

using namespace std;
using namespace boost;
using namespace boost::asio;
using namespace json_spirit;

void ThreadRPCServer2(void* parg);

// Key used by getwork/getblocktemplate miners.
// Allocated in StartRPCThreads, free'd in StopRPCThreads
CReserveKey* pMiningKey = NULL;

static std::string strRPCUserColonPass;

static int64 nWalletUnlockTime;
static CCriticalSection cs_nWalletUnlockTime;

extern Value dumpprivkey(const Array& params, bool fHelp);
extern Value importprivkey(const Array& params, bool fHelp);
extern Value importpassphrase(const Array& params, bool fHelp);
extern int64 GetBurnTxTotal();
Object JSONRPCError(int code, const string& message)
{
    Object error;
    error.push_back(Pair("code", code));
    error.push_back(Pair("message", message));
    return error;
}
void RPCTypeCheck(const Array& params,
                  const list<Value_type>& typesExpected)
{
    unsigned int i = 0;
    BOOST_FOREACH(Value_type t, typesExpected)
    {
        if (params.size() <= i)
            break;

       const Value& v = params[i];
        if (v.type() != t)
        {
            string err = strprintf("Expected type %s, got %s",
                                   Value_type_name[t], Value_type_name[v.type()]);
            throw JSONRPCError(-3, err);
        }
        i++;
    }
}
void RPCTypeCheck(const Object& o,
                  const map<string, Value_type>& typesExpected)
{
    BOOST_FOREACH(const PAIRTYPE(string, Value_type)& t, typesExpected)
    {
        const Value& v = find_value(o, t.first);
        if (v.type() == null_type)
            throw JSONRPCError(-3, strprintf("Missing %s", t.first.c_str()));
        if (v.type() != t.second)
        {
            string err = strprintf("Expected type %s for %s, got %s",
                                   Value_type_name[t.second], t.first.c_str(), Value_type_name[v.type()]);
            throw JSONRPCError(-3, err);
        }
    }
}

double GetDifficulty(const CBlockIndex* blockindex = NULL)
{
    // Floating point number that is a multiple of the minimum difficulty,
    // minimum difficulty = 1.0.
    if (blockindex == NULL)
    {
        if (pindexBest == NULL)
            return 1.0;
        else
            blockindex = GetLastBlockIndex(pindexBest, false);
    }

    int nShift = (blockindex->nBits >> 24) & 0xff;

    double dDiff =
        (double)0x0000ffff / (double)(blockindex->nBits & 0x00ffffff);

    while (nShift < 29)
    {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29)
    {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

double GetNetworkGhps(const CBlockIndex* blockindex = NULL)
{
    // Floating point number that is a multiple of the minimum difficulty,
    // minimum difficulty = 1.0.
    if (blockindex == NULL)
    {
        if (pindexBest == NULL)
            return 1.0;
        else
            blockindex = GetLastBlockIndex(pindexBest, false);
    }

    int64 nTargetSpacingWorkMin = 30;
    int64 nTargetSpacingWork = nTargetSpacingWorkMin;
    int64 nInterval = 72;
    CBlockIndex* pindex = pindexGenesisBlock;
    CBlockIndex* pindexPrevWork = pindexGenesisBlock;
    while (pindex)
    {
        // Exponential moving average of recent proof-of-work block spacing
        if (pindex->IsProofOfWork())
        {
            //obtain the time between the blocks
            int64 nActualSpacingWork = pindex->GetBlockTime() - pindexPrevWork->GetBlockTime();
            nTargetSpacingWork = ((nInterval - 1) * nTargetSpacingWork + nActualSpacingWork + nActualSpacingWork) / (nInterval + 1);
            nTargetSpacingWork = max(nTargetSpacingWork, nTargetSpacingWorkMin);
            pindexPrevWork = pindex;
        }
        pindex = pindex->pnext;
    }
    double dNetworkGhps = GetDifficulty() * 4.294967296 / nTargetSpacingWork; 
    return dNetworkGhps;
}


int64 GetBurnTxTotal()
{
    int blockstogoback = pindexBest->nHeight;
    int64 totalburnedcoins = 0;

    const CBlockIndex* pindexFirst = pindexBest;
    for (int i = 0; pindexFirst && i < blockstogoback; i++) {

        CBlock block;
        block.ReadFromDisk(pindexFirst, true, false);

        BOOST_FOREACH (const CTransaction& tx, block.vtx)
        {
            std::string txmsg;
            bool isBroadcast;
            CTransaction ctx = tx;
            if ( tx.IsBurnTx() ) {
                totalburnedcoins += tx.GetBurnOutTx().nValue;
            }
        }

        pindexFirst = pindexFirst->pprev;
    }
    return totalburnedcoins;
}


void RPCTypeCheck(const Array& params, const list<Value_type>& typesExpected, bool fAllowNull)
{
    unsigned int i = 0;
    BOOST_FOREACH(Value_type t, typesExpected)
    {
        if (params.size() <= i)
            break;

        const Value& v = params[i];
        if (!((v.type() == t) || (fAllowNull && (v.type() == null_type))))
        {
            string err = strprintf("Expected type %s, got %s",
                                                         Value_type_name[t], Value_type_name[v.type()]);
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
        i++;
    }
}

void RPCTypeCheck(const Object& o, const map<string, Value_type>& typesExpected, bool fAllowNull)
{
    BOOST_FOREACH(const PAIRTYPE(string, Value_type)& t, typesExpected)
    {
        const Value& v = find_value(o, t.first);
        if (!fAllowNull && v.type() == null_type)
            throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Missing %s", t.first.c_str()));

        if (!((v.type() == t.second) || (fAllowNull && (v.type() == null_type))))
        {
            string err = strprintf("Expected type %s for %s, got %s",
                                                         Value_type_name[t.second], t.first.c_str(), Value_type_name[v.type()]);
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
    }
}

int64 AmountFromValue(const Value& value)
{
    double dAmount = value.get_real();
    if (dAmount <= 0.0 || dAmount > MAX_MONEY)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
    int64 nAmount = roundint64(dAmount * COIN);
    if (!MoneyRange(nAmount))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
    return nAmount;
}

Value ValueFromAmount(int64 amount)
{
    return (double)amount / (double)COIN;
}

std::string HexBits(unsigned int nBits)
{
    union {
        int32_t nBits;
        char cBits[4];
    } uBits;
    uBits.nBits = htonl((int32_t)nBits);
    return HexStr(BEGIN(uBits.cBits), END(uBits.cBits));
}

void WalletTxToJSON(const CWalletTx& wtx, Object& entry)
{
    int confirms = wtx.GetDepthInMainChain();
    entry.push_back(Pair("confirmations", confirms));
    if (confirms)
    {
        entry.push_back(Pair("blockhash", wtx.hashBlock.GetHex()));
        entry.push_back(Pair("blockindex", wtx.nIndex));
    }
    entry.push_back(Pair("txid", wtx.GetHash().GetHex()));
    entry.push_back(Pair("time", (boost::int64_t)wtx.GetTxTime()));
    BOOST_FOREACH(const PAIRTYPE(string,string)& item, wtx.mapValue)
        entry.push_back(Pair(item.first, item.second));
}

string AccountFromValue(const Value& value)
{
    string strAccount = value.get_str();
    if (strAccount == "*")
        throw JSONRPCError(RPC_WALLET_INVALID_ACCOUNT_NAME, "Invalid account name");
    return strAccount;
}

void ScriptPubKeyToJSON(const CScript& scriptPubKey, Object& out)
{
    txnouttype type;
    vector<CTxDestination> addresses;
    int nRequired;

    out.push_back(Pair("asm", scriptPubKey.ToString()));
    out.push_back(Pair("hex", HexStr(scriptPubKey.begin(), scriptPubKey.end())));

    if (!ExtractDestinations(scriptPubKey, type, addresses, nRequired))
    {
        out.push_back(Pair("type", GetTxnOutputType(type)));
        return;
    }

    out.push_back(Pair("reqSigs", nRequired));
    out.push_back(Pair("type", GetTxnOutputType(type)));

    Array a;
    BOOST_FOREACH(const CTxDestination& addr, addresses)
        a.push_back(CBitcoinAddress(addr).ToString());
    out.push_back(Pair("addresses", a));
}

void TxToJSON(const CTransaction& tx, const uint256 hashBlock, Object& entry)
{
    entry.push_back(Pair("txid", tx.GetHash().GetHex()));
    entry.push_back(Pair("version", tx.nVersion));
    entry.push_back(Pair("time", (boost::int64_t)tx.nTime));
    entry.push_back(Pair("locktime", (boost::int64_t)tx.nLockTime));
    bool isBurnTx = tx.IsBurnTx();
    entry.push_back(Pair("IsBurnTx", isBurnTx));

    Array vin;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        Object in;
        if (tx.IsCoinBase())
            in.push_back(Pair("coinbase", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
        else
        {
            in.push_back(Pair("txid", txin.prevout.hash.GetHex()));
            in.push_back(Pair("vout", (boost::int64_t)txin.prevout.n));
            Object o;
            o.push_back(Pair("asm", txin.scriptSig.ToString()));
            o.push_back(Pair("hex", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
            in.push_back(Pair("scriptSig", o));
        }
        in.push_back(Pair("sequence", (boost::int64_t)txin.nSequence));
        vin.push_back(in);
    }
    entry.push_back(Pair("vin", vin));
    Array vout;
    for (unsigned int i = 0; i < tx.vout.size(); i++)
    {
        const CTxOut& txout = tx.vout[i];
        Object out;
        out.push_back(Pair("value", ValueFromAmount(txout.nValue)));
        out.push_back(Pair("n", (boost::int64_t)i));
        Object o;
        ScriptPubKeyToJSON(txout.scriptPubKey, o);
        out.push_back(Pair("scriptPubKey", o));
        vout.push_back(out);
    }
    entry.push_back(Pair("vout", vout));

    if (hashBlock != 0)
    {
        entry.push_back(Pair("blockhash", hashBlock.GetHex()));
        map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second)
        {
            CBlockIndex* pindex = (*mi).second;
            if (pindex->IsInMainChain())
            {
                entry.push_back(Pair("confirmations", 1 + nBestHeight - pindex->nHeight));

                //print the effective burn coins left if this is a burnTx
                if (isBurnTx)
                {
                        entry.push_back(Pair("burnt coins", (int64_t)tx.GetBurnOutTx().nValue));
                        entry.push_back(Pair("effective burnt coins left", 
                                                                 (int64_t)tx.EffectiveBurntCoinsLeft(pindex->nHeight)));
                }

                entry.push_back(Pair("time", (int64_t)pindex->nTime));
                entry.push_back(Pair("blocktime", (boost::int64_t)pindex->nTime));
            }
            else
                entry.push_back(Pair("confirmations", 0));
        }
    }
}

void TxToJSON(const CTransaction& tx, Object& txdata)
{
    // tx data
    txdata.push_back(Pair("txid", tx.GetHash().ToString().c_str()));
    txdata.push_back(Pair("version", (int)tx.nVersion));
    txdata.push_back(Pair("locktime", (int)tx.nLockTime));
    txdata.push_back(Pair("is_coinbase", tx.IsCoinBase()));
    txdata.push_back(Pair("is_coinstake", tx.IsCoinStake()));

    // add inputs
    Array vins;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        Object vin;

        if (txin.prevout.IsNull()) 
        {
            vin.push_back(Pair("coinbase", HexStr(txin.scriptSig).c_str()));
        }
        else 
        {
            vin.push_back(Pair("txid", txin.prevout.hash.ToString().c_str()));
            vin.push_back(Pair("vout", (int)txin.prevout.n));
            Object o;
            o.push_back(Pair("asm", txin.scriptSig.ToString()));
            o.push_back(Pair("hex", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
            vin.push_back(Pair("scriptSig", o));
        }

        vin.push_back(Pair("sequence", (boost::uint64_t)txin.nSequence));

        vins.push_back(vin);
    }
    txdata.push_back(Pair("vin", vins));

    // add outputs
    Array vouts;
    int n = 0;
    BOOST_FOREACH(const CTxOut& txout, tx.vout)
    {
        Object vout;

        std::vector<CTxDestination> addresses;
        txnouttype txtype;
        int nRequired;

        vout.push_back(Pair("value", ValueFromAmount(txout.nValue)));
        vout.push_back(Pair("n", n));

        Object scriptpubkey;

        scriptpubkey.push_back(Pair("asm", txout.scriptPubKey.ToString()));
        scriptpubkey.push_back(Pair("hex", HexStr(txout.scriptPubKey.begin(), txout.scriptPubKey.end())));

        if (ExtractDestinations(txout.scriptPubKey, txtype, addresses, nRequired))
        {
            scriptpubkey.push_back(Pair("type", GetTxnOutputType(txtype)));
            scriptpubkey.push_back(Pair("reqSig", nRequired));

            Array addrs;
            BOOST_FOREACH(const CTxDestination& addr, addresses)
                addrs.push_back(CBitcoinAddress(addr).ToString());
            scriptpubkey.push_back(Pair("addresses", addrs));
        }
        else
        {
            scriptpubkey.push_back(Pair("type", GetTxnOutputType(TX_NONSTANDARD)));
        }

        vout.push_back(Pair("scriptPubKey",scriptpubkey));

        vouts.push_back(vout);   
        n++;             
    }
    txdata.push_back(Pair("vout", vouts));
}

/*
  {
    "@id": "http://purl.org/net/bel-epa/ccy#C12ab2d1d006d48f33d4a424b716b2d7680f71235de83307d0fe56a718257c457",
    "@type": [ "http://purl.org/net/bel-epa/ccy#Block" ],
    "http://purl.org/net/bel-epa/ccy#difficulty": [{"@type": "http://www.w3.org/2001/XMLSchema#decimal", "@value": "0.03124954"}],
    "http://purl.org/net/bel-epa/ccy#flags": [{"@value": "proof-of-stake"}],
    "http://purl.org/net/bel-epa/ccy#height": [{"@value": 556100}],
    "http://purl.org/net/bel-epa/ccy#merkleroot": [{"@type": "http://www.w3.org/2001/XMLSchema#hexBinary", "@value": "1887d6e254448fea88ed77b84e9336910f2b1baef5a8e182cc265625f6175379"}],
    "http://purl.org/net/bel-epa/ccy#mint": [{"@type": "http://www.w3.org/2001/XMLSchema#decimal", "@value": "0.775923"}],
    "http://purl.org/net/bel-epa/ccy#nextblockhash": [{"@id": "http://purl.org/net/bel-epa/ccy#C0000001ead83577b254c3812c8072806d549e1572b435fc6a5be021839cf5082"}],
    "http://purl.org/net/bel-epa/ccy#previousblockhash": [{"@id": "http://purl.org/net/bel-epa/ccy#C0000004770bd40c11d827ea5d437298be84e4ea2fece9495ff96a899479dd4b4"}],
    "http://purl.org/net/bel-epa/ccy#size": [{"@value": 544}],
    "http://purl.org/net/bel-epa/ccy#time": [{"@value": 1452318302}]
  }
*/
Object blockToJSON(const CBlock& block, const CBlockIndex* blockindex, bool fTxInfo, bool fTxDetails)
{
    Object result;
    result.push_back(Pair("hash", block.GetHash().GetHex()));
    result.push_back(Pair("size", (int)::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION)));
    // "http://purl.org/net/bel-epa/ccy#size": [{"@value": "{}"}],
    result.push_back(Pair("height", blockindex->nHeight));
    // "http://purl.org/net/bel-epa/ccy#height": [{"@value": "{blockindex->nHeight}"}],
    result.push_back(Pair("version", block.nVersion));
    // "http://purl.org/net/bel-epa/ccy#version": [{"@value": "{block.nVersion}"}],
    result.push_back(Pair("merkleroot", block.hashMerkleRoot.GetHex()));
    // "http://purl.org/net/bel-epa/ccy#merkelroot": [{"@type": "http://www.w3.org/2001/XMLSchema#hexBinary", "@value": "{block.hashMerkleRoot.GetHex()}"}],
    result.push_back(Pair("time", DateTimeStrFormat(block.GetBlockTime())));
    // "http://purl.org/net/bel-epa/ccy#time": [{"@value": "{DateTimeStrFormat(block.GetBlockTime())}"}],
    result.push_back(Pair("nonce", (boost::uint64_t)block.nNonce));
    // "http://purl.org/net/bel-epa/ccy#nonce": [{"@value": "{(boost::uint64_t)block.nNonce)}"}],
    result.push_back(Pair("bits", HexBits(block.nBits)));
    // "http://purl.org/net/bel-epa/ccy#bits": [{"@type": "http://www.w3.org/2001/XMLSchema#hexBinary", "@value": "{HexBits(block.nBits)}"}],
    result.push_back(Pair("difficulty", GetDifficulty(blockindex)));
    // "http://purl.org/net/bel-epa/ccy#difficulty": [{"@type": "http://www.w3.org/2001/XMLSchema#decimal", "@value": "{GetDifficulty(blockindex)}"}],
    result.push_back(Pair("mint", ValueFromAmount(blockindex->nMint)));
    // "http://purl.org/net/bel-epa/ccy#mint": [{"@type": "http://www.w3.org/2001/XMLSchema#decimal", "@value": "{ValueFromAmount(blockindex->nMint)}"}],
    result.push_back(Pair("burnt", ValueFromAmount(blockindex->burnt)));
    // "http://purl.org/net/bel-epa/ccy#burnt": [{"@type": "http://www.w3.org/2001/XMLSchema#decimal", "@value": "{ValueFromAmount(blockindex->nMint)}"}],

    if (blockindex->pprev)
        result.push_back(Pair("previousblockhash", blockindex->pprev->GetBlockHash().GetHex()));
        // "http://purl.org/net/bel-epa/ccy#previousblockhash": [{"@id": "http://purl.org/net/bel-epa/ccy#C{blockindex->pnext->GetBlockHash().GetHex()}"}],

    if (blockindex->pnext)
        result.push_back(Pair("nextblockhash", blockindex->pnext->GetBlockHash().GetHex()));
        // "http://purl.org/net/bel-epa/ccy#nextblockhash": [{"@id": "http://purl.org/net/bel-epa/ccy#C{blockindex->pnext->GetBlockHash().GetHex()}"}],

    {
        string flags, proofhash;

        if (blockindex->IsProofOfBurn())
        {
            flags += "proof-of-burn";

            result.push_back(Pair("Intermediate PoB hash", blockindex->burnHash.GetHex()));
            proofhash += block.GetBurnHash(false).GetHex();
        }
        else if (blockindex->IsProofOfStake())
        {
            flags += "proof-of-stake";
            flags += blockindex->GeneratedStakeModifier() ? " stake-modifier": "";

            proofhash += blockindex->hashProofOfStake.GetHex();
        }
        else
        { 
            //IsProofOfWork()
            flags += "proof-of-work";

            proofhash += blockindex->GetBlockHash().GetHex();
        }
  
        result.push_back(Pair("flags", flags));
        // "http://purl.org/net/bel-epa/ccy#flags": [{"@value": "{flags}"}],
        result.push_back(Pair("proofhash", proofhash));
        // "http://purl.org/net/bel-epa/ccy#proofhash": [{"@value": "{proofhash}"}],
    }

    result.push_back(Pair("entropybit", (int)blockindex->GetStakeEntropyBit()));
        // "http://purl.org/net/bel-epa/ccy#entropybit": [{"@value": "{blockindex->GetStakeEntropyBit()}"}],
    result.push_back(Pair("modifier", strprintf("%016llx", blockindex->nStakeModifier)));
        // "http://purl.org/net/bel-epa/ccy#modifier": [{"@value": "{blockindex->nStakeModifier}"}],
    result.push_back(Pair("modifierchecksum", strprintf("%08x", blockindex->nStakeModifierChecksum)));
        // "http://purl.org/net/bel-epa/ccy#modifierchecksum": [{"@value": "{blockindex->nStakeModifierChecksum}"}],

    //PoB details
    if (blockindex->IsProofOfBurn())
    {
        result.push_back(Pair("burnBlkHeight", blockindex->burnBlkHeight));
        // "http://purl.org/net/bel-epa/ccy#burnblkheight": [{"@value": "{blockindex->burnBlkHeight}"}],
        result.push_back(Pair("burnCTx", blockindex->burnCTx));
        // "http://purl.org/net/bel-epa/ccy#burnctx": [{"@value": "{blockindex->burnCTx}"}],
        result.push_back(Pair("burnCTxOut", blockindex->burnCTxOut));
        // "http://purl.org/net/bel-epa/ccy#burnctxout": [{"@value": "{blockindex->burnCTxOut}"}],
    }

    result.push_back(Pair("nEffectiveBurnCoins", strprintf("%lld", blockindex->nEffectiveBurnCoins)));
        // "http://purl.org/net/bel-epa/ccy#neffectiveburncoins": [{"@value": "{blockindex->nEffectiveBurnCoins}"}],
    result.push_back(Pair("Formatted nEffectiveBurnCoins", FormatMoney(blockindex->nEffectiveBurnCoins)));
    result.push_back(Pair("nBurnBits", HexBits(blockindex->nBurnBits)));
        // "http://purl.org/net/bel-epa/ccy#nburnbits": [{"@value": "{HexBits(blockindex->nBurnBits}"}],

    Array txinfo;
    BOOST_FOREACH (const CTransaction& tx, block.vtx)
    {
        if (fTxInfo && !fTxDetails)
        {
            txinfo.push_back(tx.ToStringShort());
            txinfo.push_back(DateTimeStrFormat(tx.nTime));
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
                txinfo.push_back(txin.ToStringShort());
            BOOST_FOREACH(const CTxOut& txout, tx.vout)
                txinfo.push_back(txout.ToStringShort());
        }
        else if (fTxDetails) 
        {
            Object txdata;
            TxToJSON(tx, txdata);
            txinfo.push_back(txdata);
        }
        else
            txinfo.push_back(tx.GetHash().GetHex());
    }
    result.push_back(Pair("tx", txinfo));
    // "http://purl.org/net/bel-epa/ccy#tx": [{"@id": "http://purl.org/net/bel-epa/ccy#C{txinfo}"],
    return result;
}

string HelpRequiringPassphrase()
{
    return pwalletMain->IsCrypted()
        ? "requires wallet passphrase to be set with walletpassphrase first"
        : "";
}

void EnsureWalletIsUnlocked()
{
    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    if (fWalletUnlockMintOnly)
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Wallet is unlocked for Proof of Stake only.");
}


///
/// Note: This interface may still be subject to change.
///

string CRPCTable::help(string strCommand) const
{
    string strRet;
    set<rpcfn_type> setDone;
    for (map<string, const CRPCCommand*>::const_iterator mi = mapCommands.begin(); mi != mapCommands.end(); ++mi)
    {
        const CRPCCommand *pcmd = mi->second;
        string strMethod = mi->first;
        // We already filter duplicates, but these deprecated screw up the sort order
        if (strMethod == "getamountreceived" ||
            strMethod == "getallreceived" ||
            strMethod == "getblocknumber" || // deprecated
            (strMethod.find("label") != string::npos))
            continue;
        if (strCommand != "" && strMethod != strCommand)
            continue;
        try
        {
            Array params;
            rpcfn_type pfn = pcmd->actor;
            if (setDone.insert(pfn).second)
                (*pfn)(params, true);
        }
        catch (std::exception& e)
        {
            // Help text is returned in an exception
            string strHelp = string(e.what());
            if (strCommand == "")
                if (strHelp.find('\n') != string::npos)
                    strHelp = strHelp.substr(0, strHelp.find('\n'));
            strRet += strHelp + "\n";
        }
    }
    if (strRet == "")
        strRet = strprintf("help: unknown command: %s\n", strCommand.c_str());
    strRet = strRet.substr(0,strRet.size()-1);
    return strRet;
}

Value help(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "help [command]\n"
            "List commands, or get help for a command.");

    string strCommand;
    if (params.size() > 0)
        strCommand = params[0].get_str();

    return tableRPC.help(strCommand);
}

Value stop(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "stop\n"
            "Stop slimcoin server.");
    // Shutdown will take long enough that the response should get back
    StartShutdown();
    return "slimcoin server stopping";
}

Value getblockcount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getblockcount\n"
            "Returns the number of blocks in the longest block chain.");

    return nBestHeight;
}

// deprecated
Value getblocknumber(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getblocknumber\n"
            "Deprecated.  Use getblockcount.");

    return nBestHeight;
}

Value getconnectioncount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getconnectioncount\n"
            "Returns the number of connections to other nodes.");

    LOCK(cs_vNodes);
    return (int)vNodes.size();
}

static void CopyNodeStats(std::vector<CNodeStats>& vstats)
{
    vstats.clear();

    LOCK(cs_vNodes);
    vstats.reserve(vNodes.size());
    BOOST_FOREACH(CNode* pnode, vNodes) {
        CNodeStats stats;
        pnode->copyStats(stats);
        vstats.push_back(stats);
    }
}

Value getpeerinfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getpeerinfo\n"
            "Returns data about each connected network node.");

    vector<CNodeStats> vstats;
    CopyNodeStats(vstats);

    Array ret;

    BOOST_FOREACH(const CNodeStats& stats, vstats) {
        Object obj;

        obj.push_back(Pair("addr", stats.addrName));
        obj.push_back(Pair("services", strprintf("%08llx", stats.nServices)));
        obj.push_back(Pair("lastsend", (boost::int64_t)stats.nLastSend));
        obj.push_back(Pair("lastrecv", (boost::int64_t)stats.nLastRecv));
        obj.push_back(Pair("conntime", (boost::int64_t)stats.nTimeConnected));
        obj.push_back(Pair("version", stats.nVersion));
        obj.push_back(Pair("subver", stats.strSubVer));
        obj.push_back(Pair("inbound", stats.fInbound));
        obj.push_back(Pair("releasetime", (boost::int64_t)stats.nReleaseTime));
        obj.push_back(Pair("height", stats.nStartingHeight));
        obj.push_back(Pair("banscore", stats.nMisbehavior));

        ret.push_back(obj);
    }
    
    return ret;
}

Value addnode(const Array& params, bool fHelp)
{
    string strCommand;
    if (params.size() == 2)
        strCommand = params[1].get_str();
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "addnode <node>\n"
            "Attempts to try a connection to <node> once.");

    string strAddr = params[0].get_str();

    CAddress addr(CService(strAddr, GetDefaultPort(), fAllowDNS));
    addr.nTime = 0; // so it won't relay unless successfully connected

    if (addr.IsValid()) {
        ConnectNode(addr);
        return "done";
    }
    return Value::null;
}

Value getdifficulty(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getdifficulty\n"
            "Returns difficulty as a multiple of the minimum difficulty.");

    Object obj;
    obj.push_back(Pair("proof-of-work",        GetDifficulty()));
    obj.push_back(Pair("proof-of-stake",       GetDifficulty(GetLastBlockIndex(pindexBest, true))));
    obj.push_back(Pair("search-interval",      (int)nLastCoinStakeSearchInterval));
    return obj;
}

Value getgenerate(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getgenerate\n"
            "Returns true or false.");

    return GetBoolArg("-gen");
}

Value setgenerate(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "setgenerate <generate> [genproclimit]\n"
            "<generate> is true or false to turn generation on or off.\n"
            "Generation is limited to [genproclimit] processors, -1 is unlimited.");

    bool fGenerate = true;
    if (params.size() > 0)
        fGenerate = params[0].get_bool();

    if (params.size() > 1)
    {
        int nGenProcLimit = params[1].get_int();
        mapArgs["-genproclimit"] = itostr(nGenProcLimit);
        if (nGenProcLimit == 0)
            fGenerate = false;
    }
    mapArgs["-gen"] = (fGenerate ? "1" : "0");

    GenerateSlimcoins(fGenerate, pwalletMain);
    return Value::null;
}

Value gethashespersec(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "gethashespersec\n"
            "Returns a recent hashes per second performance measurement while generating.");

    if (GetTimeMillis() - nHPSTimerStart > 8000)
        return (boost::int64_t)0;
    return (boost::int64_t)dHashesPerSec;
}

// ppcoin: get network Gh/s estimate
Value getnetworkghps(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getnetworkghps\n"
            "Returns a recent Ghash/second network mining estimate.");

    int64 nTargetSpacingWorkMin = 30;
    int64 nTargetSpacingWork = nTargetSpacingWorkMin;
    int64 nInterval = 72;
    CBlockIndex* pindex = pindexGenesisBlock;
    CBlockIndex* pindexPrevWork = pindexGenesisBlock;
    while (pindex)
    {
        // Exponential moving average of recent proof-of-work block spacing
        if (pindex->IsProofOfWork())
        {
            //obtain the time between the blocks
            int64 nActualSpacingWork = pindex->GetBlockTime() - pindexPrevWork->GetBlockTime();
            nTargetSpacingWork = ((nInterval - 1) * nTargetSpacingWork + nActualSpacingWork + nActualSpacingWork) / (nInterval + 1);
            nTargetSpacingWork = max(nTargetSpacingWork, nTargetSpacingWorkMin);
            pindexPrevWork = pindex;
        }
        pindex = pindex->pnext;
    }
    double dNetworkGhps = GetDifficulty() * 4.294967296 / nTargetSpacingWork; 
    return dNetworkGhps;
}

Value getinfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getinfo\n"
            "Returns an object containing various state info.");

    Object obj;
    obj.push_back(Pair("version",       FormatFullVersion()));
    obj.push_back(Pair("protocolversion",(int)PROTOCOL_VERSION));
    obj.push_back(Pair("walletversion", pwalletMain->GetVersion()));
    obj.push_back(Pair("balance",       ValueFromAmount(pwalletMain->GetBalance())));
    obj.push_back(Pair("newmint",       ValueFromAmount(pwalletMain->GetNewMint())));
    obj.push_back(Pair("stake",         ValueFromAmount(pwalletMain->GetStake())));
    obj.push_back(Pair("blocks",        (int)nBestHeight));
    obj.push_back(Pair("moneysupply",   ValueFromAmount(pindexBest->nMoneySupply)));
    obj.push_back(Pair("burnt",         ValueFromAmount(pindexBest->burnt)));
    //obj.push_back(Pair("burnt",         FormatMoney(GetBurnTxTotal())));
    obj.push_back(Pair("connections",   (int)vNodes.size()));
    obj.push_back(Pair("proxy",         (fUseProxy ? addrProxy.ToStringIPPort() : string())));
    obj.push_back(Pair("ip",            addrSeenByPeer.ToStringIP()));
    obj.push_back(Pair("difficulty",    (double)GetDifficulty()));
    obj.push_back(Pair("testnet",       fTestNet));
    obj.push_back(Pair("keypoololdest", (boost::int64_t)pwalletMain->GetOldestKeyPoolTime()));
    obj.push_back(Pair("keypoolsize",   pwalletMain->GetKeyPoolSize()));
    obj.push_back(Pair("paytxfee",      ValueFromAmount(nTransactionFee)));
    if (pwalletMain->IsCrypted())
        obj.push_back(Pair("unlocked_until", (boost::int64_t)nWalletUnlockTime / 1000));
    obj.push_back(Pair("errors",        GetWarnings("statusbar")));
    return obj;
}

Value getmininginfo(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getmininginfo\n"
            "Returns an object containing mining-related information.");

    Object obj;
    obj.push_back(Pair("blocks",        (int)nBestHeight));
    obj.push_back(Pair("currentblocksize",(uint64_t)nLastBlockSize));
    obj.push_back(Pair("currentblocktx",(uint64_t)nLastBlockTx));
    obj.push_back(Pair("difficulty",    (double)GetDifficulty()));
    obj.push_back(Pair("errors",        GetWarnings("statusbar")));
    obj.push_back(Pair("generate",      GetBoolArg("-gen")));
    obj.push_back(Pair("genproclimit",  (int)GetArg("-genproclimit", -1)));
    obj.push_back(Pair("hashespersec",  gethashespersec(params, false)));
    obj.push_back(Pair("networkghps",   getnetworkghps(params, false)));
    obj.push_back(Pair("pooledtx",      (uint64_t)mempool.size()));
    obj.push_back(Pair("testnet",       fTestNet));
    return obj;
}

Value getnewaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getnewaddress [account]\n"
            "Returns a new slimcoin address for receiving payments.  "
            "If [account] is specified (recommended), it is added to the address book "
            "so payments received with the address will be credited to [account].");

    // Parse the account first so we don't generate a key if there's an error
    string strAccount;
    if (params.size() > 0)
        strAccount = AccountFromValue(params[0]);

    if (!pwalletMain->IsLocked())
        pwalletMain->TopUpKeyPool();

    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!pwalletMain->GetKeyFromPool(newKey, false))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    CKeyID keyID = newKey.GetID();

    pwalletMain->SetAddressBookName(keyID, strAccount);

    return CBitcoinAddress(keyID).ToString();
}

CBitcoinAddress GetAccountAddress(string strAccount, bool bForceNew=false)
{
    CWalletDB walletdb(pwalletMain->strWalletFile);

    CAccount account;
    walletdb.ReadAccount(strAccount, account);

    bool bKeyUsed = false;

    // Check if the current key has been used
    if (account.vchPubKey.IsValid())
    {
        CScript scriptPubKey;
        scriptPubKey.SetDestination(account.vchPubKey.GetID());
        for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin();
             it != pwalletMain->mapWallet.end() && account.vchPubKey.IsValid();
             ++it)
        {
            const CWalletTx& wtx = (*it).second;
            BOOST_FOREACH(const CTxOut& txout, wtx.vout)
                if (txout.scriptPubKey == scriptPubKey)
                    bKeyUsed = true;
        }
    }

    // Generate a new key
    if (!account.vchPubKey.IsValid() || bForceNew || bKeyUsed)
    {
        if (!pwalletMain->GetKeyFromPool(account.vchPubKey, false))
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");

        pwalletMain->SetAddressBookName(account.vchPubKey.GetID(), strAccount);
        walletdb.WriteAccount(strAccount, account);
    }

    return CBitcoinAddress(account.vchPubKey.GetID());
}

Value getaccountaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaccountaddress <account>\n"
            "Returns the current slimcoin address for receiving payments to this account.");

    // Parse the account first so we don't generate a key if there's an error
    string strAccount = AccountFromValue(params[0]);

    Value ret;

    ret = GetAccountAddress(strAccount).ToString();

    return ret;
}

Value setaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "setaccount <slimcoinaddress> <account>\n"
            "Sets the account associated with the given address.");

    CBitcoinAddress address(params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid slimcoin address");


    string strAccount;
    if (params.size() > 1)
        strAccount = AccountFromValue(params[1]);

    // Detect when changing the account of an address that is the 'unused current key' of another account:
    if (pwalletMain->mapAddressBook.count(address.Get()))
    {
        string strOldAccount = pwalletMain->mapAddressBook[address.Get()];
        if (address == GetAccountAddress(strOldAccount))
            GetAccountAddress(strOldAccount, true);
    }

    pwalletMain->SetAddressBookName(address.Get(), strAccount);

    return Value::null;
}

Value getaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaccount <slimcoinaddress>\n"
            "Returns the account associated with the given address.");

    CBitcoinAddress address(params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid slimcoin address");

    string strAccount;
    map<CTxDestination, string>::iterator mi = pwalletMain->mapAddressBook.find(address.Get());
    if (mi != pwalletMain->mapAddressBook.end() && !(*mi).second.empty())
        strAccount = (*mi).second;
    return strAccount;
}

Value getaddressesbyaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getaddressesbyaccount <account>\n"
            "Returns the list of addresses for the given account.");

    string strAccount = AccountFromValue(params[0]);

    // Find all addresses that have the given account
    Array ret;
    BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
    {
        const CBitcoinAddress& address = item.first;
        const string& strName = item.second;
        if (strName == strAccount)
            ret.push_back(address.ToString());
    }
    return ret;
}

Value settxfee(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1 || AmountFromValue(params[0]) < MIN_TX_FEE)
        throw runtime_error(
            "settxfee <amount>\n"
            "<amount> is a real and is rounded to 0.01 (cent)\n"
            "Minimum and default transaction fee per KB is 1 cent");

    nTransactionFee = AmountFromValue(params[0]);
    nTransactionFee = (nTransactionFee / CENT) * CENT;  // round to cent
    return true;
}

Value sendtoaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 5)
        throw runtime_error(
            "sendtoaddress <slimcoinaddress> <amount> [comment] [comment-to]\n"
            "<amount> is a real and is rounded to the nearest 0.000001\n"
            + HelpRequiringPassphrase());

    CBitcoinAddress address(params[0].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid slimcoin address");

    //the address should not be a burn address
    if (IsBurnAddress(address, true))
        throw JSONRPCError(RPC_INVALID_PARAMETER, string("Sending coins to burn address without using burncoins command"));

    // Amount
    int64 nAmount = AmountFromValue(params[1]);
    if (nAmount < MIN_TXOUT_AMOUNT)
        throw JSONRPCError(RPC_SEND_AMOUNT_TOO_SMALL, "Send amount too small");

    // Wallet comments
    CWalletTx wtx;
    if (params.size() > 2 && params[2].type() != null_type && !params[2].get_str().empty())
        wtx.mapValue["comment"] = params[2].get_str();
    if (params.size() > 3 && params[3].type() != null_type && !params[3].get_str().empty())
        wtx.mapValue["to"]      = params[3].get_str();

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, wtx);
    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    return wtx.GetHash().GetHex();
}

Value signmessage(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw runtime_error(
            "signmessage <slimcoinaddress> <message>\n"
            "Sign a message with the private key of an address");

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    string strAddress = params[0].get_str();
    string strMessage = params[1].get_str();

    CBitcoinAddress addr(strAddress);
    if (!addr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID;
    if (!addr.GetKeyID(keyID))
        throw JSONRPCError(-3, "Address does not refer to key");

    CKey key;
    if (!pwalletMain->GetKey(keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

    CDataStream ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    vector<unsigned char> vchSig;
    if (!key.SignCompact(Hash(ss.begin(), ss.end()), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    return EncodeBase64(&vchSig[0], vchSig.size());
}

Value verifymessage(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 3)
        throw runtime_error(
            "verifymessage <slimcoinaddress> <signature> <message>\n"
            "Verify a signed message");

    string strAddress  = params[0].get_str();
    string strSign     = params[1].get_str();
    string strMessage  = params[2].get_str();

    CBitcoinAddress addr(strAddress);
    if (!addr.IsValid())
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    CKeyID keyID;
    if (!addr.GetKeyID(keyID))
        throw JSONRPCError(-3, "Address does not refer to key");

    bool fInvalid = false;
    vector<unsigned char> vchSig = DecodeBase64(strSign.c_str(), &fInvalid);

    if (fInvalid)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

    CDataStream ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CKey key;
    if (!key.SetCompactSignature(Hash(ss.begin(), ss.end()), vchSig))
        return false;

    return (key.GetPubKey().GetID() == keyID);
}

Value getreceivedbyaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getreceivedbyaddress <slimcoinaddress> [minconf=1]\n"
            "Returns the total amount received by <slimcoinaddress> in transactions with at least [minconf] confirmations.");

    // Bitcoin address
    CBitcoinAddress address = CBitcoinAddress(params[0].get_str());
    CScript scriptPubKey;
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid slimcoin address");
    scriptPubKey.SetDestination(address.Get());
    if (!IsMine(*pwalletMain,scriptPubKey))
        return (double)0.0;

    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 1)
        nMinDepth = params[1].get_int();

    // Tally
    int64 nAmount = 0;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        if (wtx.IsCoinBase() || wtx.IsCoinStake() || !wtx.IsFinal())
            continue;

        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
            if (txout.scriptPubKey == scriptPubKey)
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
    }

    return  ValueFromAmount(nAmount);
}

void GetAccountAddresses(string strAccount, set<CTxDestination>& setAddress)
{
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, string)& item, pwalletMain->mapAddressBook)
    {
        const CTxDestination& address = item.first;
        const string& strName = item.second;
        if (strName == strAccount)
            setAddress.insert(address);
    }
}

Value getreceivedbyaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getreceivedbyaccount <account> [minconf=1]\n"
            "Returns the total amount received by addresses with <account> in transactions with at least [minconf] confirmations.");

    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 1)
        nMinDepth = params[1].get_int();

    // Get the set of pub keys assigned to account
    string strAccount = AccountFromValue(params[0]);
    set<CTxDestination> setAddress;
    GetAccountAddresses(strAccount, setAddress);

    // Tally
    int64 nAmount = 0;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        if (wtx.IsCoinBase() || wtx.IsCoinStake() || !wtx.IsFinal())
            continue;

        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
        {
            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*pwalletMain, address) && setAddress.count(address))
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
        }
    }

    return (double)nAmount / (double)COIN;
}

int64 GetAccountBalance(CWalletDB& walletdb, const string& strAccount, int nMinDepth)
{
    int64 nBalance = 0;

    // Tally wallet transactions
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        if (!wtx.IsFinal())
            continue;

        int64 nGenerated, nReceived, nSent, nFee;
        wtx.GetAccountAmounts(strAccount, nGenerated, nReceived, nSent, nFee);

        if (nReceived != 0 && wtx.GetDepthInMainChain() >= nMinDepth)
            nBalance += nReceived;
        nBalance += nGenerated - nSent - nFee;
    }

    // Tally internal accounting entries
    nBalance += walletdb.GetAccountCreditDebit(strAccount);

    return nBalance;
}

int64 GetAccountBalance(const string& strAccount, int nMinDepth)
{
    CWalletDB walletdb(pwalletMain->strWalletFile);
    return GetAccountBalance(walletdb, strAccount, nMinDepth);
}

Value getbalance(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "getbalance [account] [minconf=1]\n"
            "If [account] is not specified, returns the server's total available balance.\n"
            "If [account] is specified, returns the balance in the account.");

    if (params.size() == 0)
        return ValueFromAmount(pwalletMain->GetBalance());

    int nMinDepth = 1;
    if (params.size() > 1)
        nMinDepth = params[1].get_int();

    if (params[0].get_str() == "*") {
        // Calculate total balance a different way from GetBalance()
        // (GetBalance() sums up all unspent TxOuts)
        // getbalance and getbalance '*' should always return the same number.
        int64 nBalance = 0;
        for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
        {
            const CWalletTx& wtx = (*it).second;
            if (!wtx.IsFinal())
                continue;

            int64 allGeneratedImmature, allGeneratedMature, allFee;
            allGeneratedImmature = allGeneratedMature = allFee = 0;
            string strSentAccount;
            list<pair<CTxDestination, int64> > listReceived;
            list<pair<CTxDestination, int64> > listSent;
            wtx.GetAmounts(allGeneratedImmature, allGeneratedMature, listReceived, listSent, allFee, strSentAccount);
            if (wtx.GetDepthInMainChain() >= nMinDepth)
            {
                BOOST_FOREACH(const PAIRTYPE(CTxDestination,int64)& r, listReceived)
                    nBalance += r.second;
            }
            BOOST_FOREACH(const PAIRTYPE(CTxDestination,int64)& r, listSent)
                nBalance -= r.second;
            nBalance -= allFee;
            nBalance += allGeneratedMature;
        }
        return  ValueFromAmount(nBalance);
    }

    string strAccount = AccountFromValue(params[0]);

    int64 nBalance = GetAccountBalance(strAccount, nMinDepth);

    return ValueFromAmount(nBalance);
}

Value movecmd(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 3 || params.size() > 5)
        throw runtime_error(
            "move <fromaccount> <toaccount> <amount> [minconf=1] [comment]\n"
            "Move from one account in your wallet to another.");

    string strFrom = AccountFromValue(params[0]);
    string strTo = AccountFromValue(params[1]);
    int64 nAmount = AmountFromValue(params[2]);
    if (params.size() > 3)
        // unused parameter, used to be nMinDepth, keep type-checking it though
        (void)params[3].get_int();
    string strComment;
    if (params.size() > 4)
        strComment = params[4].get_str();

    CWalletDB walletdb(pwalletMain->strWalletFile);
    if (!walletdb.TxnBegin())
        throw JSONRPCError(RPC_DATABASE_ERROR, "database error");

    int64 nNow = GetAdjustedTime();

    // Debit
    CAccountingEntry debit;
    debit.strAccount = strFrom;
    debit.nCreditDebit = -nAmount;
    debit.nTime = nNow;
    debit.strOtherAccount = strTo;
    debit.strComment = strComment;
    walletdb.WriteAccountingEntry(debit);

    // Credit
    CAccountingEntry credit;
    credit.strAccount = strTo;
    credit.nCreditDebit = nAmount;
    credit.nTime = nNow;
    credit.strOtherAccount = strFrom;
    credit.strComment = strComment;
    walletdb.WriteAccountingEntry(credit);

    if (!walletdb.TxnCommit())
        throw JSONRPCError(RPC_DATABASE_ERROR, "database error");

    return true;
}

Value calcburnhash(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
            "calcburnhash [fPrintRegardless=false]\n"
            "Returns the smallest hash of all of the burn transactions\n"
            "If fPrintRegardless is true, it returns the smallest hash\n"
            "\tregardless if it was able to be calculated");

    bool fPrintRegardless = false;
    if (params.size() > 0)
        fPrintRegardless = params[0].get_bool();

    uint256 smallestHash;
    CWalletTx smallestWTx;
    
    HashAllBurntTx(smallestHash, smallestWTx);

    string output = "";
    if (smallestHash == ~uint256(0))
    {
        //if there are no burnt coins or the best block in the chain is a PoB block
        if (pwalletMain->setBurnHashes.empty())
            output += "Found no burnt coins or coins are not mature enough\n";

        if (pindexBest->IsProofOfBurn())
            output += "Last block in chain is a proof-of-burn block\n";

        if (!fPrintRegardless)
            return output += "Unable to calculate the smallest burn hash";
        else
            output += "Unable to calculate the smallest burn hash, printing regardless...\n\n";
    }

    output += "Smallest Hash is:   " + smallestHash.GetHex() + "\n";
    output += "By transaction id:  " + smallestWTx.GetHash().GetHex() + "\n";
    output += "Target:             " + CBigNum().SetCompact(pindexBest->nBurnBits).getuint256().GetHex() + "\n";
    output += strprintf("nBurnBits=%08x, nEffectiveBurnCoins=%llu (formatted %s)",
                                            pindexBest->nBurnBits, pindexBest->nEffectiveBurnCoins, 
                                            FormatMoney(pindexBest->nEffectiveBurnCoins).c_str());

    return output;
}

Value burncoins(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 5)
        throw runtime_error(
            "burncoins <fromaccount> <amount> [minconf=1] [comment] [comment-to]\n"
            "<amount> is a real and is rounded to the nearest 0.000001\n"
            + HelpRequiringPassphrase());

    string strAccount = AccountFromValue(params[0]);
    
    CBurnAddress burnAddress;

    int64 nAmount = AmountFromValue(params[1]);

    if (nAmount < MIN_TXOUT_AMOUNT)
        throw JSONRPCError(RPC_SEND_AMOUNT_TOO_SMALL, "Send amount too small");

    int nMinDepth = 1;
    if (params.size() > 2)
        nMinDepth = params[2].get_int();

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;

    if (params.size() > 3 && params[3].type() != null_type && !params[3].get_str().empty())
        wtx.mapValue["comment"] = params[3].get_str();
    if (params.size() > 4 && params[4].type() != null_type && !params[4].get_str().empty())
        wtx.mapValue["to"]      = params[4].get_str();

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                                             "Error: Please enter the wallet passphrase with walletpassphrase first.");

    // Check funds
    int64 nBalance = GetAccountBalance(strAccount, nMinDepth);
    if (nAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    // Send
    /* FIXME: sanity check required */
    string strError = pwalletMain->SendMoneyToDestination(burnAddress.Get(), nAmount, wtx, false, true);
    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    return wtx.GetHash().GetHex();
}

Array getBurnCoinBalances(int64& netBurnCoins, int64& nEffBurnCoins, int64& immatureCoins)
{
    Array ret;
    netBurnCoins = nEffBurnCoins = immatureCoins = 0;

    BOOST_FOREACH(const uint256 &hash, pwalletMain->setBurnHashes)
    {
        Object entry;
        const CWalletTx &wtx = pwalletMain->mapWallet[hash];
        CTxOut outTx = wtx.GetBurnOutTx();

        if (outTx.IsNull())
            continue;

        s32int burnConfirms = wtx.GetBurnDepthInMainChain();    

        //fill the entry
        entry.push_back(Pair("burned amount", ValueFromAmount(outTx.nValue)));    
        entry.push_back(Pair("burn confirmations", burnConfirms));

        //wtx.GetBurnDepthInMainChain() must be >= BURN_MIN_CONFIRMS for the burn transaction to be mature
        s32int mature = burnConfirms - BURN_MIN_CONFIRMS;

        if (mature < 0)
            entry.push_back(Pair("burnt coins immature, burn confirmations needed", -1 * mature));

        WalletTxToJSON(wtx, entry);

        //record the burnt coins
        netBurnCoins += outTx.nValue;

        //if they are mature, add to the nEffBurnCoins, else add to the immatureCoins
        if (mature >= 0)
            nEffBurnCoins += BurnCalcEffectiveCoins(outTx.nValue, mature);
        else
            immatureCoins += outTx.nValue;

        ret.push_back(entry);
    }

    return ret;
}

Value getburndata(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error("getburndata\n"
                            "Lists useful proof-of-burn information");

    int64 netBurnCoins, nEffBurnCoins, immatureCoins;
 
    Array ret = getBurnCoinBalances(netBurnCoins, nEffBurnCoins, immatureCoins);

    Object entry;
    entry.push_back(Pair("Net Burnt Coins", ValueFromAmount(netBurnCoins)));
    entry.push_back(Pair("Effective Burnt Coins", ValueFromAmount(nEffBurnCoins)));
    entry.push_back(Pair("Immature Burnt Coins", ValueFromAmount(immatureCoins)));
    entry.push_back(Pair("Decayed Burnt Coins", ValueFromAmount(netBurnCoins - immatureCoins - nEffBurnCoins)));
    ret.push_back(entry);

    Object info;
    info.push_back(Pair("General Info", ""));
    info.push_back(Pair("nBurnBits", strprintf("%08llx", pindexBest->nBurnBits)));
    info.push_back(Pair("nEffectiveBurnCoins", strprintf("%lld", pindexBest->nEffectiveBurnCoins)));
    info.push_back(Pair("Formatted nEffectiveBurnCoins", FormatMoney(pindexBest->nEffectiveBurnCoins)));
                                 
    ret.push_back(info);

    return ret;
}

Value sendfrom(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 3 || params.size() > 7)
        throw runtime_error(
            "sendfrom <fromaccount> <toslimcoinaddress> <amount> [minconf=1] [comment] [comment-to]\n"
            "<amount> is a real and is rounded to the nearest 0.000001\n"
            + HelpRequiringPassphrase());

    string strAccount = AccountFromValue(params[0]);
    CBitcoinAddress address(params[1].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid slimcoin address");

    //the address should not be a burn address
    if (IsBurnAddress(address, true))
        throw JSONRPCError(RPC_INVALID_PARAMETER, string("Sending coins to burn address without using burncoins command"));

    int64 nAmount = AmountFromValue(params[2]);
    if (nAmount < MIN_TXOUT_AMOUNT)
        throw JSONRPCError(RPC_SEND_AMOUNT_TOO_SMALL, "Send amount too small");

    int nMinDepth = 1;
    if (params.size() > 3)
        nMinDepth = params[3].get_int();

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;
    if (params.size() > 4 && params[4].type() != null_type && !params[4].get_str().empty())
        wtx.mapValue["comment"] = params[4].get_str();
    if (params.size() > 5 && params[5].type() != null_type && !params[5].get_str().empty())
        wtx.mapValue["to"]      = params[5].get_str();

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    // Check funds
    int64 nBalance = GetAccountBalance(strAccount, nMinDepth);
    if (nAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    // Send
    string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, wtx);
    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    return wtx.GetHash().GetHex();
}

Value sendmany(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 4)
        throw runtime_error(
            "sendmany <fromaccount> {address:amount,...} [minconf=1] [comment]\n"
            "amounts are double-precision floating point numbers\n"
            + HelpRequiringPassphrase());

    string strAccount = AccountFromValue(params[0]);
    Object sendTo = params[1].get_obj();
    int nMinDepth = 1;
    if (params.size() > 2)
        nMinDepth = params[2].get_int();

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;
    if (params.size() > 3 && params[3].type() != null_type && !params[3].get_str().empty())
        wtx.mapValue["comment"] = params[3].get_str();

    set<CBitcoinAddress> setAddress;
    vector<pair<CScript, int64> > vecSend;

    int64 totalAmount = 0;
    BOOST_FOREACH(const Pair& s, sendTo)
    {
        CBitcoinAddress address(s.name_);
        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid slimcoin address:")+s.name_);

        //the address should not be a burn address
        if (IsBurnAddress(address, true))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Sending coins to burn address without using burncoins command"));

        if (setAddress.count(address))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+s.name_);
        setAddress.insert(address);

        CScript scriptPubKey;
        scriptPubKey.SetDestination(address.Get());
        int64 nAmount = AmountFromValue(s.value_); 
        if (nAmount < MIN_TXOUT_AMOUNT)
            throw JSONRPCError(RPC_SEND_AMOUNT_TOO_SMALL, "Send amount too small");
        totalAmount += nAmount;

        vecSend.push_back(make_pair(scriptPubKey, nAmount));
    }

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
    if (fWalletUnlockMintOnly)
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Wallet unlocked for block minting only.");

    // Check funds
    int64 nBalance = GetAccountBalance(strAccount, nMinDepth);
    if (totalAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    // Send
    CReserveKey keyChange(pwalletMain);
    int64 nFeeRequired = 0;
    bool fCreated = pwalletMain->CreateTransaction(vecSend, wtx, keyChange, nFeeRequired);
    if (!fCreated)
    {
        if (totalAmount + nFeeRequired > pwalletMain->GetBalance())
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");
        throw JSONRPCError(RPC_WALLET_ERROR, "Transaction creation failed");
    }
    if (!pwalletMain->CommitTransaction(wtx, keyChange))
        throw JSONRPCError(RPC_WALLET_ERROR, "Transaction commit failed");

    return wtx.GetHash().GetHex();
}

/*
Value addmultisigaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
    {
        string msg = "addmultisigaddress <nrequired> <'[\"key\",\"key\"]'> [account]\n"
            "Add a nrequired-to-sign multisignature address to the wallet\"\n"
            "each key is a bitcoin address or hex-encoded public key\n"
            "If [account] is specified, assign address to [account].";
        throw runtime_error(msg);
    }

    int nRequired = params[0].get_int();
    const Array& keys = params[1].get_array();
    string strAccount;
    if (params.size() > 2)
        strAccount = AccountFromValue(params[2]);

    // Gather public keys
    if (nRequired < 1)
        throw runtime_error("a multisignature address must require at least one key to redeem");
    if ((int)keys.size() < nRequired)
        throw runtime_error(
            strprintf("not enough keys supplied "
                      "(got %d keys, but need at least %d to redeem)", keys.size(), nRequired));
    std::vector<CPubKey> pubkeys;
    pubkeys.resize(keys.size());
    for (unsigned int i = 0; i < keys.size(); i++)
    {
        const std::string& ks = keys[i].get_str();

        // Case 1: bitcoin address and we have full public key:
        CBitcoinAddress address(ks);
        if (address.IsValid())
        {
            CKeyID keyID;
            if (!address.GetKeyID(keyID))
                throw runtime_error(
                    strprintf("%s does not refer to a key",ks.c_str()));
            CPubKey vchPubKey;
            if (!pwalletMain->GetPubKey(keyID, vchPubKey))
                throw runtime_error(
                    strprintf("no full public key for address %s",ks.c_str()));
            if (!vchPubKey.IsValid() || !pubkeys[i].SetPubKey(vchPubKey))
                throw runtime_error(" Invalid public key: "+ks);
        }

        // Case 2: hex public key
        else if (IsHex(ks))
        {
            CPubKey vchPubKey(ParseHex(ks));
            if (!vchPubKey.IsValid() || !pubkeys[i].SetPubKey(vchPubKey))
                throw runtime_error(" Invalid public key: "+ks);
        }
        else
        {
            throw runtime_error(" Invalid public key: "+ks);
        }
    }

    // Construct using pay-to-script-hash:
    CScript inner;
    inner.SetMultisig(nRequired, pubkeys);
    CScriptID innerID = inner.GetID();
    pwalletMain->AddCScript(inner);

    pwalletMain->SetAddressBookName(innerID, strAccount);
    return CBitcoinAddress(innerID).ToString();
}
*/

//
// Used by addmultisigaddress / createmultisig:
//
CScript _createmultisig(const Array& params)
{
    int nRequired = params[0].get_int();
    const Array& keys = params[1].get_array();

    // Gather public keys
    if (nRequired < 1)
        throw runtime_error("a multisignature address must require at least one key to redeem");
    if ((int)keys.size() < nRequired)
        throw runtime_error(strprintf("not enough keys supplied (got %u keys, but need at least %d to redeem)", keys.size(), nRequired));
    std::vector<CPubKey> pubkeys;
    pubkeys.resize(keys.size());
    for (unsigned int i = 0; i < keys.size(); i++)
    {
        const std::string& ks = keys[i].get_str();

        // Case 1: Bitcoin address and we have full public key:
        CBitcoinAddress address(ks);
        if (pwalletMain && address.IsValid())
        {
            CKeyID keyID;
            if (!address.GetKeyID(keyID))
                throw runtime_error(strprintf("%s does not refer to a key",ks.c_str()));
            CPubKey vchPubKey;
            if (!pwalletMain->GetPubKey(keyID, vchPubKey))
                throw runtime_error(strprintf("no full public key for address %s",ks.c_str()));
            if (!vchPubKey.IsFullyValid())
                throw runtime_error(" Invalid public key: "+ks);
            pubkeys[i] = vchPubKey;
        }

        // Case 2: hex public key
        else if (IsHex(ks))
        {
            CPubKey vchPubKey(ParseHex(ks));
            if (!vchPubKey.IsFullyValid())
                throw runtime_error(" Invalid public key: "+ks);
            pubkeys[i] = vchPubKey;
        }
        else
        {
            throw runtime_error(" Invalid public key: "+ks);
        }
    }
    CScript result;
    result.SetMultisig(nRequired, pubkeys);
    return result;
}

Value addmultisigaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 3)
    {
        string msg = "addmultisigaddress <nrequired> <'[\"key\",\"key\"]'> [account]\n"
            "Add a nrequired-to-sign multisignature address to the wallet\n"
            "each key is a Slimcoin address or hex-encoded public key\n"
            "If [account] is specified, assign address to [account].";
        throw runtime_error(msg);
    }

    string strAccount;
    if (params.size() > 2)
        strAccount = AccountFromValue(params[2]);

    // Construct using pay-to-script-hash:
    CScript inner = _createmultisig(params);
    CScriptID innerID = inner.GetID();
    pwalletMain->AddCScript(inner);

    pwalletMain->SetAddressBookName(innerID, strAccount);
    return CBitcoinAddress(innerID).ToString();
}

Value createmultisig(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 2 || params.size() > 2)
    {
        string msg = "createmultisig nrequired [\"key\",...]\n"
            "\nCreates a multi-signature address with n signature of m keys required.\n"
            "It returns a json object with the address and redeemScript.\n"

            "\nArguments:\n"
            "1. nrequired (numeric, required) The number of required signatures out of the n keys or addresses.\n"
            "2. \"keys\" (string, required) A json array of keys which are peercoin addresses or hex-encoded public keys\n"
            " [\n"
            " \"key\" (string) Slimcoin address or hex-encoded public key\n"
            " ,...\n"
            " ]\n"

            "\nResult:\n"
            "{\n"
            " \"address\":\"multisigaddress\", (string) The value of the new multisig address.\n"
            " \"redeemScript\":\"script\" (string) The string value of the hex-encoded redemption script.\n"
            "}\n"

            "\nExamples:\n"
            "\nCreate a multisig address from 2 addresses\n"
            "slimcoind createmultisig 2 \"[\\\"PCHAhUGKiFKDHKW8Pgw3qrp2vMfhwWjuCo\\\",\\\"PJrhyo8CUvFZQT8j67Expre2PYLhavnHXb\\\"]\""
            "\nAs a json rpc call\n"
            "curl --user myusername --data-binary '{\"jsonrpc\": \"1.0\", \"id\": \"curltest\", \"method\": \"icreatemultisig\", \"params\": [2, \"[\\\"PCHAhUGKiFKDHKW8Pgw3qrp2vMfhwWjuCo\\\",\\\"PJrhyo8CUvFZQT8j67Expre2PYLhavnHXb\\\"]\"]} -H 'content-type: text/plain;' http://127.0.0.1:9902"
        ;
        throw runtime_error(msg);
    }

    // Construct using pay-to-script-hash:
    CScript inner = _createmultisig(params);
    CScriptID innerID = inner.GetID();
    CBitcoinAddress address(innerID);

    Object result;
    result.push_back(Pair("address", address.ToString()));
    result.push_back(Pair("redeemScript", HexStr(inner.begin(), inner.end())));

    return result;
}


struct tallyitem
{
    int64 nAmount;
    int nConf;
    tallyitem()
    {
        nAmount = 0;
        nConf = std::numeric_limits<int>::max();
    }
};

Value ListReceived(const Array& params, bool fByAccounts)
{
    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 0)
        nMinDepth = params[0].get_int();

    // Whether to include empty accounts
    bool fIncludeEmpty = false;
    if (params.size() > 1)
        fIncludeEmpty = params[1].get_bool();

    // Tally
    map<CBitcoinAddress, tallyitem> mapTally;
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;

        if (wtx.IsCoinBase() || wtx.IsCoinStake() || !wtx.IsFinal())
            continue;

        int nDepth = wtx.GetDepthInMainChain();
        if (nDepth < nMinDepth)
            continue;

        BOOST_FOREACH(const CTxOut& txout, wtx.vout)
        {
            CTxDestination address;
            if (!ExtractDestination(txout.scriptPubKey, address) || !IsMine(*pwalletMain, address))
                continue;

            tallyitem& item = mapTally[address];
            item.nAmount += txout.nValue;
            item.nConf = min(item.nConf, nDepth);
        }
    }

    // Reply
    Array ret;
    map<string, tallyitem> mapAccountTally;
    BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
    {
        const CBitcoinAddress& address = item.first;
        const string& strAccount = item.second;
        map<CBitcoinAddress, tallyitem>::iterator it = mapTally.find(address);
        if (it == mapTally.end() && !fIncludeEmpty)
            continue;

        int64 nAmount = 0;
        int nConf = std::numeric_limits<int>::max();
        if (it != mapTally.end())
        {
            nAmount = (*it).second.nAmount;
            nConf = (*it).second.nConf;
        }

        if (fByAccounts)
        {
            tallyitem& item = mapAccountTally[strAccount];
            item.nAmount += nAmount;
            item.nConf = min(item.nConf, nConf);
        }
        else
        {
            Object obj;
            obj.push_back(Pair("address",       address.ToString()));
            obj.push_back(Pair("account",       strAccount));
            obj.push_back(Pair("amount",        ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            ret.push_back(obj);
        }
    }

    if (fByAccounts)
    {
        for (map<string, tallyitem>::iterator it = mapAccountTally.begin(); it != mapAccountTally.end(); ++it)
        {
            int64 nAmount = (*it).second.nAmount;
            int nConf = (*it).second.nConf;
            Object obj;
            obj.push_back(Pair("account",       (*it).first));
            obj.push_back(Pair("amount",        ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            ret.push_back(obj);
        }
    }

    return ret;
}

Value listreceivedbyaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "listreceivedbyaddress [minconf=1] [includeempty=false]\n"
            "[minconf] is the minimum number of confirmations before payments are included.\n"
            "[includeempty] whether to include addresses that haven't received any payments.\n"
            "Returns an array of objects containing:\n"
            "  \"address\" : receiving address\n"
            "  \"account\" : the account of the receiving address\n"
            "  \"amount\" : total amount received by the address\n"
            "  \"confirmations\" : number of confirmations of the most recent transaction included");

    return ListReceived(params, false);
}

Value listreceivedbyaccount(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "listreceivedbyaccount [minconf=1] [includeempty=false]\n"
            "[minconf] is the minimum number of confirmations before payments are included.\n"
            "[includeempty] whether to include accounts that haven't received any payments.\n"
            "Returns an array of objects containing:\n"
            "  \"account\" : the account of the receiving addresses\n"
            "  \"amount\" : total amount received by addresses with this account\n"
            "  \"confirmations\" : number of confirmations of the most recent transaction included");

    return ListReceived(params, true);
}

static bool listBurnMintedSort(pair<Object, s32int> i, pair<Object, s32int> j)
{
    return i.second > j.second;
}

Value listburnminted(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "listburnminted [verbose=false]\n"
            "Returns the blocks and respective rewards for all\n"
            "\tProof-of-Burn blocks found by this wallet.");

    bool fVerbose = false;
    if (params.size() > 0)
        fVerbose = params[0].get_bool();

    vector<pair<Object, s32int> > entries;

    for(map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); 
            it != pwalletMain->mapWallet.end(); ++it)
    {
        CWalletTx wtx = it->second;
        CBlockIndex *pindex;
        s32int confirms;

        //check if this transaction is in the main chain
        confirms = wtx.GetDepthInMainChain(pindex);
        if (!confirms)
            continue;

        //check if the transaction is a coin base transaction and this block is a PoB block
        if (!wtx.IsCoinBase() || !pindex->IsProofOfBurn())
            continue;

        Object entry;
        entry.push_back(Pair("PoB Block Hash", pindex->GetBlockHash().GetHex()));
        entry.push_back(Pair("PoB Hash", pindex->burnHash.GetHex()));


        /***Unused variables only needed as function arguments for wtx.GetAmounts()***/
        int64 nFee;
        string strSentAccount;
        list<pair<CTxDestination, int64> > listReceived;
        list<pair<CTxDestination, int64> > listSent;
        /***Unused variables only needed as function arguments for wtx.GetAmounts()***/

        int64 nGeneratedImmature, nGeneratedMature;    
        wtx.GetAmounts(nGeneratedImmature, nGeneratedMature, listReceived, listSent, nFee, strSentAccount);

        //Reward is immature
        if (nGeneratedImmature)
        {
            entry.push_back(Pair("category", "immature"));
            entry.push_back(Pair("amount", ValueFromAmount(nGeneratedImmature)));
        }else{ //reward is mature
            entry.push_back(Pair("category", "mint by burn"));
            entry.push_back(Pair("amount", ValueFromAmount(nGeneratedMature)));
        }

        if (fVerbose)
            WalletTxToJSON(wtx, entry);

        entries.push_back(make_pair(entry, confirms));

    }

    //sort the entries, from largest amount of confirmations to the smallest
    sort(entries.begin(), entries.end(), listBurnMintedSort);

    //take each entry and make an Array of it
    Array ret;

    BOOST_FOREACH(PAIRTYPE(Object, int) &entry, entries)
        ret.push_back(entry.first);

    return ret;
}

void ListTransactions(const CWalletTx& wtx, const string& strAccount, int nMinDepth, bool fLong, Array& ret)
{
    int64 nGeneratedImmature, nGeneratedMature, nFee;
    string strSentAccount;
    list<pair<CTxDestination, int64> > listReceived;
    list<pair<CTxDestination, int64> > listSent;

    wtx.GetAmounts(nGeneratedImmature, nGeneratedMature, listReceived, listSent, nFee, strSentAccount);

    bool fAllAccounts = (strAccount == string("*"));

    // Generated blocks assigned to account ""
    if ((nGeneratedMature+nGeneratedImmature) != 0 && (fAllAccounts || strAccount == ""))
    {
        Object entry;
        entry.push_back(Pair("account", string("")));
        if (nGeneratedImmature)
        {
            entry.push_back(Pair("category", wtx.GetDepthInMainChain() ? "immature" : "orphan"));
            entry.push_back(Pair("amount", ValueFromAmount(nGeneratedImmature)));
        }
        else
        {
            entry.push_back(Pair("category", "generate"));
            entry.push_back(Pair("amount", ValueFromAmount(nGeneratedMature)));
        }
        if (fLong)
            WalletTxToJSON(wtx, entry);
        ret.push_back(entry);
    }

    // Sent
    if ((!listSent.empty() || nFee != 0) && (fAllAccounts || strAccount == strSentAccount))
    {
        BOOST_FOREACH(const PAIRTYPE(CTxDestination, int64)& s, listSent)
        {
            Object entry;
            entry.push_back(Pair("account", strSentAccount));
            entry.push_back(Pair("address", CBitcoinAddress(s.first).ToString()));
            entry.push_back(Pair("category", "send"));
            entry.push_back(Pair("amount", ValueFromAmount(-s.second)));
            entry.push_back(Pair("fee", ValueFromAmount(-nFee)));
            if (fLong)
                WalletTxToJSON(wtx, entry);
            ret.push_back(entry);
        }
    }

    // Received
    if (listReceived.size() > 0 && wtx.GetDepthInMainChain() >= nMinDepth)
    {
        BOOST_FOREACH(const PAIRTYPE(CTxDestination, int64)& r, listReceived)
        {
            string account;
            if (pwalletMain->mapAddressBook.count(r.first))
                account = pwalletMain->mapAddressBook[r.first];
            if (fAllAccounts || (account == strAccount))
            {
                Object entry;
                entry.push_back(Pair("account", account));
                entry.push_back(Pair("address", CBitcoinAddress(r.first).ToString()));
                entry.push_back(Pair("category", "receive"));
                entry.push_back(Pair("amount", ValueFromAmount(r.second)));
                if (fLong)
                    WalletTxToJSON(wtx, entry);
                ret.push_back(entry);
            }
        }
    }
}

void AcentryToJSON(const CAccountingEntry& acentry, const string& strAccount, Array& ret)
{
    bool fAllAccounts = (strAccount == string("*"));

    if (fAllAccounts || acentry.strAccount == strAccount)
    {
        Object entry;
        entry.push_back(Pair("account", acentry.strAccount));
        entry.push_back(Pair("category", "move"));
        entry.push_back(Pair("time", (boost::int64_t)acentry.nTime));
        entry.push_back(Pair("amount", ValueFromAmount(acentry.nCreditDebit)));
        entry.push_back(Pair("otheraccount", acentry.strOtherAccount));
        entry.push_back(Pair("comment", acentry.strComment));
        ret.push_back(entry);
    }
}

Value listtransactions(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 4)
        throw runtime_error(
            "listtransactions [account=\"*\"] [fBurnTx=false] [count=10] [from=0]\n"
            "Returns up to [count] most recent transactions\n"
            "\t(if fBurnTx is 'true', returns only the burn transactions)\n"
            "skipping the first [from] transactions for account [account].");

    string strAccount = "*";
    if (params.size() > 0)
        strAccount = params[0].get_str();

    bool fBurnTx = false;
    if (params.size() > 1)
        fBurnTx = params[1].get_bool();

    int nCount = 10;
    if (params.size() > 2)
        nCount = params[2].get_int();

    int nFrom = 0;
    if (params.size() > 3)
        nFrom = params[3].get_int();

    if (nCount < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count");
    if (nFrom < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative from");

    Array ret;
    CWalletDB walletdb(pwalletMain->strWalletFile);

    // First: get all CWalletTx and CAccountingEntry into a sorted-by-time multimap.
    typedef pair<CWalletTx*, CAccountingEntry*> TxPair;
    typedef multimap<int64, TxPair > TxItems;
    TxItems txByTime;

    // Note: maintaining indices in the database of (account,time) --> txid and (account, time) --> acentry
    // would make this much faster for applications that do this a lot.
    if (fBurnTx)
    {
        for (set<uint256>::iterator it = pwalletMain->setBurnHashes.begin(); 
                it != pwalletMain->setBurnHashes.end(); ++it)
        {
            CWalletTx* wtx = &(pwalletMain->mapWallet.at(*it));
            txByTime.insert(make_pair(wtx->GetTxTime(), TxPair(wtx, (CAccountingEntry*)0)));
        }
    }
    else
    {
        for(map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
        {
            CWalletTx* wtx = &((*it).second);
            txByTime.insert(make_pair(wtx->GetTxTime(), TxPair(wtx, (CAccountingEntry*)0)));
        }
    }

    list<CAccountingEntry> acentries;
    walletdb.ListAccountCreditDebit(strAccount, acentries);
    BOOST_FOREACH(CAccountingEntry& entry, acentries)
    {
        txByTime.insert(make_pair(entry.nTime, TxPair((CWalletTx*)0, &entry)));
    }

    // iterate backwards until we have nCount items to return:
    for (TxItems::reverse_iterator it = txByTime.rbegin(); it != txByTime.rend(); ++it)
    {
        CWalletTx *const pwtx = (*it).second.first;
        if (pwtx != 0)
            ListTransactions(*pwtx, strAccount, 0, true, ret);
        CAccountingEntry *const pacentry = (*it).second.second;
        if (pacentry != 0)
            AcentryToJSON(*pacentry, strAccount, ret);

        if (ret.size() >= (nCount+nFrom)) break;
    }
    // ret is newest to oldest
    
    if (nFrom > (int)ret.size())
        nFrom = ret.size();
    if ((nFrom + nCount) > (int)ret.size())
        nCount = ret.size() - nFrom;
    Array::iterator first = ret.begin();
    std::advance(first, nFrom);
    Array::iterator last = ret.begin();
    std::advance(last, nFrom+nCount);

    if (last != ret.end()) ret.erase(last, ret.end());
    if (first != ret.begin()) ret.erase(ret.begin(), first);

    std::reverse(ret.begin(), ret.end()); // Return oldest to newest

    return ret;
}

Value listaccounts(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "listaccounts [minconf=1]\n"
            "Returns Object that has account names as keys, account balances as values.");

    int nMinDepth = 1;
    if (params.size() > 0)
        nMinDepth = params[0].get_int();

    map<string, int64> mapAccountBalances;
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, string)& entry, pwalletMain->mapAddressBook) {
        if (IsMine(*pwalletMain, entry.first)) // This address belongs to me
            mapAccountBalances[entry.second] = 0;
    }

    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it)
    {
        const CWalletTx& wtx = (*it).second;
        int64 nGeneratedImmature, nGeneratedMature, nFee;
        string strSentAccount;
        list<pair<CTxDestination, int64> > listReceived;
        list<pair<CTxDestination, int64> > listSent;
        wtx.GetAmounts(nGeneratedImmature, nGeneratedMature, listReceived, listSent, nFee, strSentAccount);
        mapAccountBalances[strSentAccount] -= nFee;
        BOOST_FOREACH(const PAIRTYPE(CTxDestination, int64)& s, listSent)
            mapAccountBalances[strSentAccount] -= s.second;
        if (wtx.GetDepthInMainChain() >= nMinDepth)
        {
            mapAccountBalances[""] += nGeneratedMature;
            BOOST_FOREACH(const PAIRTYPE(CTxDestination, int64)& r, listReceived)
                if (pwalletMain->mapAddressBook.count(r.first))
                    mapAccountBalances[pwalletMain->mapAddressBook[r.first]] += r.second;
                else
                    mapAccountBalances[""] += r.second;
        }
    }

    list<CAccountingEntry> acentries;
    CWalletDB(pwalletMain->strWalletFile).ListAccountCreditDebit("*", acentries);
    BOOST_FOREACH(const CAccountingEntry& entry, acentries)
        mapAccountBalances[entry.strAccount] += entry.nCreditDebit;

    Object ret;
    BOOST_FOREACH(const PAIRTYPE(string, int64)& accountBalance, mapAccountBalances) {
        ret.push_back(Pair(accountBalance.first, ValueFromAmount(accountBalance.second)));
    }
    return ret;
}

Value listsinceblock(const Array& params, bool fHelp)
{
    if (fHelp)
        throw runtime_error(
            "listsinceblock [blockhash] [target-confirmations]\n"
            "Get all transactions in blocks since block [blockhash], or all transactions if omitted");

    CBlockIndex *pindex = NULL;
    int target_confirms = 1;

    if (params.size() > 0)
    {
        uint256 blockId = 0;

        blockId.SetHex(params[0].get_str());
        pindex = CBlockLocator(blockId).GetBlockIndex();
    }

    if (params.size() > 1)
    {
        target_confirms = params[1].get_int();

        if (target_confirms < 1)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
    }

    int depth = pindex ? (1 + nBestHeight - pindex->nHeight) : -1;

    Array transactions;

    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); it++)
    {
        CWalletTx tx = (*it).second;

        if (depth == -1 || tx.GetDepthInMainChain() < depth)
            ListTransactions(tx, "*", 0, true, transactions);
    }

    uint256 lastblock;

    if (target_confirms == 1)
    {
        lastblock = hashBestChain;
    }
    else
    {
        int target_height = pindexBest->nHeight + 1 - target_confirms;

        CBlockIndex *block;
        for (block = pindexBest;
             block && block->nHeight > target_height;
             block = block->pprev)  { }

        lastblock = block ? block->GetBlockHash() : 0;
    }

    Object ret;
    ret.push_back(Pair("transactions", transactions));
    ret.push_back(Pair("lastblock", lastblock.GetHex()));

    return ret;
}

Value gettransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "gettransaction <txid>\n"
            "Get detailed information about <txid>");

    uint256 hash;
    hash.SetHex(params[0].get_str());

    Object entry;

    if (!pwalletMain->mapWallet.count(hash))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    const CWalletTx& wtx = pwalletMain->mapWallet[hash];

    int64 nCredit = wtx.GetCredit();
    int64 nDebit = wtx.GetDebit();
    int64 nNet = nCredit - nDebit;
    int64 nFee = (wtx.IsFromMe() ? wtx.GetValueOut() - nDebit : 0);

    entry.push_back(Pair("amount", ValueFromAmount(nNet - nFee)));
    if (wtx.IsFromMe())
        entry.push_back(Pair("fee", ValueFromAmount(nFee)));

    WalletTxToJSON(pwalletMain->mapWallet[hash], entry);

    Array details;
    ListTransactions(pwalletMain->mapWallet[hash], "*", 0, false, details);
    entry.push_back(Pair("details", details));

    return entry;
}

Value backupwallet(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "backupwallet <destination>\n"
            "Safely copies wallet.dat to destination, which can be a directory or a path with filename.");

    string strDest = params[0].get_str();
    BackupWallet(*pwalletMain, strDest);

    return Value::null;
}

Value keypoolrefill(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "keypoolrefill\n"
            "Fills the keypool, " + HelpRequiringPassphrase());

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

    pwalletMain->TopUpKeyPool();

    if (pwalletMain->GetKeyPoolSize() < GetArg("-keypool", 100))
        throw JSONRPCError(RPC_WALLET_ERROR, "Error refreshing keypool.");

    return Value::null;
}

void ThreadTopUpKeyPool(void* parg)
{
    pwalletMain->TopUpKeyPool();
}

void ThreadCleanWalletPassphrase(void* parg)
{
    int64 nMyWakeTime = GetTimeMillis() + *((int64*)parg) * 1000;

    ENTER_CRITICAL_SECTION(cs_nWalletUnlockTime);

    if (nWalletUnlockTime == 0)
    {
        nWalletUnlockTime = nMyWakeTime;

        do
        {
            if (nWalletUnlockTime==0)
                break;
            int64 nToSleep = nWalletUnlockTime - GetTimeMillis();
            if (nToSleep <= 0)
                break;

            LEAVE_CRITICAL_SECTION(cs_nWalletUnlockTime);
            Sleep(nToSleep);
            ENTER_CRITICAL_SECTION(cs_nWalletUnlockTime);

        } while(1);

        if (nWalletUnlockTime)
        {
            nWalletUnlockTime = 0;
            pwalletMain->LockKeyStore();
        }
    }
    else
    {
        if (nWalletUnlockTime < nMyWakeTime)
            nWalletUnlockTime = nMyWakeTime;
    }

    LEAVE_CRITICAL_SECTION(cs_nWalletUnlockTime);

    delete (int64*)parg;
}

Value walletpassphrase(const Array& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() < 2 || params.size() > 3))
        throw runtime_error(
            "walletpassphrase <passphrase> <timeout> [mintonly]\n"
            "Stores the wallet decryption key in memory for <timeout> seconds.\n"
            "mintonly is optional true/false allowing only block minting.");
    if (fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrase was called.");

    if (!pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_ALREADY_UNLOCKED, "Error: Wallet is already unlocked, use walletlock first if need to change unlock settings.");

    // Note that the walletpassphrase is stored in params[0] which is not mlock()ed
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make params[0] mlock()'d to begin with.
    strWalletPass = params[0].get_str().c_str();

    if (strWalletPass.length() > 0)
    {
        if (!pwalletMain->Unlock(strWalletPass))
            throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
    }
    else
        throw runtime_error(
            "walletpassphrase <passphrase> <timeout>\n"
            "Stores the wallet decryption key in memory for <timeout> seconds.");

    CreateThread(ThreadTopUpKeyPool, NULL);
    int64* pnSleepTime = new int64(params[1].get_int64());
    CreateThread(ThreadCleanWalletPassphrase, pnSleepTime);

    // ppcoin: if user OS account compromised prevent trivial sendmoney commands
    if (params.size() > 2)
        fWalletUnlockMintOnly = params[2].get_bool();
    else
        fWalletUnlockMintOnly = false;

    return Value::null;
}

Value walletpassphrasechange(const Array& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() != 2))
        throw runtime_error(
            "walletpassphrasechange <oldpassphrase> <newpassphrase>\n"
            "Changes the wallet passphrase from <oldpassphrase> to <newpassphrase>.");
    if (fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrasechange was called.");

    // TODO: get rid of these .c_str() calls by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make params[0] mlock()'d to begin with.
    SecureString strOldWalletPass;
    strOldWalletPass.reserve(100);
    strOldWalletPass = params[0].get_str().c_str();

    SecureString strNewWalletPass;
    strNewWalletPass.reserve(100);
    strNewWalletPass = params[1].get_str().c_str();

    if (strOldWalletPass.length() < 1 || strNewWalletPass.length() < 1)
        throw runtime_error(
            "walletpassphrasechange <oldpassphrase> <newpassphrase>\n"
            "Changes the wallet passphrase from <oldpassphrase> to <newpassphrase>.");

    if (!pwalletMain->ChangeWalletPassphrase(strOldWalletPass, strNewWalletPass))
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");

    return Value::null;
}

Value walletlock(const Array& params, bool fHelp)
{
    if (pwalletMain->IsCrypted() && (fHelp || params.size() != 0))
        throw runtime_error(
            "walletlock\n"
            "Removes the wallet encryption key from memory, locking the wallet.\n"
            "After calling this method, you will need to call walletpassphrase again\n"
            "before being able to call any methods which require the wallet to be unlocked.");
    if (fHelp)
        return true;
    if (!pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletlock was called.");

    {
        LOCK(cs_nWalletUnlockTime);
        pwalletMain->LockKeyStore();
        nWalletUnlockTime = 0;
    }

    return Value::null;
}

Value encryptwallet(const Array& params, bool fHelp)
{
    if (!pwalletMain->IsCrypted() && (fHelp || params.size() != 1))
        throw runtime_error(
            "encryptwallet <passphrase>\n"
            "Encrypts the wallet with <passphrase>.");
    if (fHelp)
        return true;
    if (pwalletMain->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an encrypted wallet, but encryptwallet was called.");

    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make params[0] mlock()'d to begin with.
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    strWalletPass = params[0].get_str().c_str();

    if (strWalletPass.length() < 1)
        throw runtime_error(
            "encryptwallet <passphrase>\n"
            "Encrypts the wallet with <passphrase>.");

    if (!pwalletMain->EncryptWallet(strWalletPass))
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: Failed to encrypt the wallet.");

    // BDB seems to have a bad habit of writing old data into
    // slack space in .dat files; that is bad if the old data is
    // unencrypted private keys.  So:
    StartShutdown();
    return "wallet encrypted; slimcoin server stopping, restart to run with encrypted wallet";
}


class DescribeAddressVisitor : public boost::static_visitor<Object>
{
public:
    Object operator()(const CNoDestination &dest) const { return Object(); }

    Object operator()(const CKeyID &keyID) const {
        Object obj;
        CPubKey vchPubKey;
        pwalletMain->GetPubKey(keyID, vchPubKey);
        obj.push_back(Pair("isscript", false));
        obj.push_back(Pair("pubkey", HexStr(vchPubKey.Raw())));
        obj.push_back(Pair("iscompressed", vchPubKey.IsCompressed()));
        return obj;
    }

    Object operator()(const CScriptID &scriptID) const {
        Object obj;
        obj.push_back(Pair("isscript", true));
        CScript subscript;
        pwalletMain->GetCScript(scriptID, subscript);
        std::vector<CTxDestination> addresses;
        txnouttype whichType;
        int nRequired;
        ExtractDestinations(subscript, whichType, addresses, nRequired);
        obj.push_back(Pair("script", GetTxnOutputType(whichType)));
        Array a;
        BOOST_FOREACH(const CTxDestination& addr, addresses)
            a.push_back(CBitcoinAddress(addr).ToString());
        obj.push_back(Pair("addresses", a));
        if (whichType == TX_MULTISIG)
            obj.push_back(Pair("sigsrequired", nRequired));
        return obj;
    }
};

Value validateaddress(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "validateaddress <slimcoinaddress>\n"
            "Return information about <slimcoinaddress>.");

    CBitcoinAddress address(params[0].get_str());
    bool isValid = address.IsValid();

    Object ret;
    ret.push_back(Pair("isvalid", isValid));
    if (isValid)
    {
        CTxDestination dest = address.Get();
        string currentAddress = address.ToString();
        ret.push_back(Pair("address", currentAddress));
        bool fMine = IsMine(*pwalletMain, dest);
        ret.push_back(Pair("ismine", fMine));
        if (fMine) {
            Object detail = boost::apply_visitor(DescribeAddressVisitor(), dest);
            ret.insert(ret.end(), detail.begin(), detail.end());
        }
        if (pwalletMain->mapAddressBook.count(dest))
            ret.push_back(Pair("account", pwalletMain->mapAddressBook[dest]));
    }
    return ret;
}

Value getwork(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getwork [data]\n"
            "If [data] is not specified, returns formatted hash data to work on:\n"
            "  \"midstate\" : precomputed hash state after hashing the first half of the data (DEPRECATED)\n" // deprecated
            "  \"data\" : block data\n"
            "  \"hash1\" : formatted hash buffer for second hash (DEPRECATED)\n" // deprecated
            "  \"target\" : little endian hash target\n"
            "If [data] is specified, tries to solve the block and returns true if it was successful.");

    if (vNodes.empty())
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Slimcoin is not connected!");

    if (IsInitialBlockDownload())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Slimcoin is downloading blocks...");

    typedef map<uint256, pair<CBlock*, CScript> > mapNewBlock_t;
    static mapNewBlock_t mapNewBlock;
    static vector<CBlock*> vNewBlock;
    static CReserveKey reservekey(pwalletMain);

    if (params.size() == 0)
    {
        // Update block
        static unsigned int nTransactionsUpdatedLast;
        static CBlockIndex* pindexPrev;
        static int64 nStart;
        static CBlock* pblock;
        if (pindexPrev != pindexBest ||
            (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 60))
        {
            if (pindexPrev != pindexBest)
            {
                // Deallocate old blocks since they're obsolete now
                mapNewBlock.clear();
                BOOST_FOREACH(CBlock* pblock, vNewBlock)
                    delete pblock;
                vNewBlock.clear();
            }

            nTransactionsUpdatedLast = nTransactionsUpdated;
            pindexPrev = pindexBest;
            nStart = GetTime();

            // Create new block
            pblock = CreateNewBlock(pwalletMain);
            if (!pblock)
                throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");
            vNewBlock.push_back(pblock);
        }

        // Update nTime
        pblock->UpdateTime(pindexPrev);
        pblock->nNonce = 0;

        // Update nExtraNonce
        static unsigned int nExtraNonce = 0;
        IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

        // Save
        mapNewBlock[pblock->hashMerkleRoot] = make_pair(pblock, pblock->vtx[0].vin[0].scriptSig);

        // Prebuild hash buffers
        char pmidstate[32];
        char pdata[128];
        char phash1[64];
        FormatHashBuffers(pblock, pmidstate, pdata, phash1);

        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

        Object result;
        result.push_back(Pair("midstate", HexStr(BEGIN(pmidstate), END(pmidstate)))); // deprecated
        result.push_back(Pair("data",     HexStr(BEGIN(pdata), END(pdata))));
        result.push_back(Pair("hash1",    HexStr(BEGIN(phash1), END(phash1)))); // deprecated
        result.push_back(Pair("target",   HexStr(BEGIN(hashTarget), END(hashTarget))));
        return result;
    }
    else
    {
        // Parse parameters
        vector<unsigned char> vchData = ParseHex(params[0].get_str());
        if (vchData.size() != 128)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
        CBlock* pdata = (CBlock*)&vchData[0];

        // Byte reverse
        for (int i = 0; i < 128 / sizeof(u32int); i++)
            ((unsigned int*)pdata)[i] = ByteReverse(((unsigned int*)pdata)[i]);

        // Get saved block
        if (!mapNewBlock.count(pdata->hashMerkleRoot))
            return false;
        CBlock* pblock = mapNewBlock[pdata->hashMerkleRoot].first;

        pblock->nTime = pdata->nTime;
        pblock->nNonce = pdata->nNonce;
        pblock->vtx[0].vin[0].scriptSig = mapNewBlock[pdata->hashMerkleRoot].second;
        pblock->hashMerkleRoot = pblock->BuildMerkleTree();

        assert(pwalletMain != NULL);
        if (!pblock->SignBlock(*pwalletMain))
            throw JSONRPCError(RPC_UNABLE_TO_SIGN_BLOCK, "Unable to sign block, wallet locked?");

        return CheckWork(pblock, *pwalletMain, reservekey);
    }
}

Value getblocktemplate(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getblocktemplate [params]\n"
            "Returns data needed to construct a block to work on:\n"
            "  \"version\" : block version\n"
            "  \"previousblockhash\" : hash of current highest block\n"
            "  \"transactions\" : contents of non-coinbase transactions that should be included in the next block\n"
            "  \"coinbaseaux\" : data that should be included in coinbase\n"
            "  \"coinbasevalue\" : maximum allowable input to coinbase transaction, including the generation award and transaction fees\n"
            "  \"target\" : hash target\n"
            "  \"mintime\" : minimum timestamp appropriate for next block\n"
            "  \"curtime\" : current timestamp\n"
            "  \"mutable\" : list of ways the block template may be changed\n"
            "  \"noncerange\" : range of valid nonces\n"
            "  \"sigoplimit\" : limit of sigops in blocks\n"
            "  \"sizelimit\" : limit of block size\n"
            "  \"bits\" : compressed target of next block\n"
            "  \"height\" : height of the next block\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.");

    std::string strMode = "template";
    if (params.size() > 0)
    {
        const Object& oparam = params[0].get_obj();
        const Value& modeval = find_value(oparam, "mode");

        if (modeval.type() == str_type)
            strMode = modeval.get_str();
        else if (modeval.type() == null_type)
        {
            //do nothing
        }
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
    }

    if (strMode != "template")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");

    {
        if (vNodes.empty())
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Slimcoin is not connected!");

        if (IsInitialBlockDownload())
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Slimcoin is downloading blocks...");

        // Update block
        static unsigned int nTransactionsUpdatedLast;
        static CBlockIndex* pindexPrev;
        static int64 nStart;
        static CBlock* pblock;
        if (pindexPrev != pindexBest ||
                (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 5))
        {
            // Clear pindexPrev so future calls make a new block, despite any failures from here on
            pindexPrev = NULL;

            // Store the pindexBest used before CreateNewBlock, to avoid races
            nTransactionsUpdatedLast = nTransactionsUpdated;
            CBlockIndex* pindexPrevNew = pindexBest;
            nStart = GetTime();

            // Create new block
            if(pblock)
            {
                delete pblock;
                pblock = NULL;
            }
            pblock = CreateNewBlock(pwalletMain, false);
            if (!pblock)
                throw JSONRPCError(-7, "Out of memory");

            // Need to update only after we know CreateNewBlock succeeded
            pindexPrev = pindexPrevNew;
        }

        // Update nTime
        pblock->UpdateTime(pindexPrev);
        pblock->nNonce = 0;

        Array transactions;
        map<uint256, int64_t> setTxIndex;
        int i = 0;
        CTxDB txdb("r");
        BOOST_FOREACH (CTransaction& tx, pblock->vtx)
        {
            uint256 txHash = tx.GetHash();
            setTxIndex[txHash] = i++;

            if (tx.IsCoinBase())
                continue;

            Object entry;

            CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
            ssTx << tx;
            entry.push_back(Pair("data", HexStr(ssTx.begin(), ssTx.end())));

            entry.push_back(Pair("hash", txHash.GetHex()));

            MapPrevTx mapInputs;
            map<uint256, CTxIndex> mapUnused;
            bool fInvalid = false;
            if (tx.FetchInputs(txdb, mapUnused, false, false, mapInputs, fInvalid))
            {
                entry.push_back(Pair("fee", (int64_t)(tx.GetValueIn(mapInputs) - tx.GetValueOut())));

                Array deps;
                BOOST_FOREACH (MapPrevTx::value_type& inp, mapInputs)
                {
                    if (setTxIndex.count(inp.first))
                        deps.push_back(setTxIndex[inp.first]);
                }
                entry.push_back(Pair("depends", deps));

                int64_t nSigOps = tx.GetLegacySigOpCount();
                nSigOps += tx.GetP2SHSigOpCount(mapInputs);
                entry.push_back(Pair("sigops", nSigOps));
            }

            transactions.push_back(entry);
        }

        Object aux;
        aux.push_back(Pair("flags", HexStr(COINBASE_FLAGS.begin(), COINBASE_FLAGS.end())));

        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

        static Array aMutable;
        if (aMutable.empty())
        {
            aMutable.push_back("time");
            aMutable.push_back("transactions");
            aMutable.push_back("prevblock");
        }

        Object result;
        result.push_back(Pair("version", pblock->nVersion));
        result.push_back(Pair("previousblockhash", pblock->hashPrevBlock.GetHex()));
        result.push_back(Pair("transactions", transactions));
        result.push_back(Pair("coinbaseaux", aux));
        result.push_back(Pair("coinbasevalue", (int64_t)pblock->vtx[0].vout[0].nValue));
        result.push_back(Pair("target", hashTarget.GetHex()));
        result.push_back(Pair("mintime", (int64_t)pindexPrev->GetMedianTimePast()+1));
        result.push_back(Pair("mutable", aMutable));
        result.push_back(Pair("noncerange", "00000000ffffffff"));
        result.push_back(Pair("sigoplimit", (int64_t)MAX_BLOCK_SIGOPS));
        result.push_back(Pair("sizelimit", (int64_t)MAX_BLOCK_SIZE));
        result.push_back(Pair("curtime", (int64_t)pblock->nTime));
        result.push_back(Pair("bits", HexBits(pblock->nBits)));
        result.push_back(Pair("height", (int64_t)(pindexPrev->nHeight+1)));

        return result;
    }
}

Value submitblock(const Array& params, bool fHelp)
{
    printf("Entered submitblock.\n");
    printf("Param size: %i", params.size());

    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "submitblock <hex data> [optional-params-obj]\n"
            "[optional-params-obj] parameter is currently ignored.\n"
            "Attempts to submit new block to network.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.");

    if (params.size() > 0) printf("Params 0: %s\n", params[0].get_str().c_str());
    if (params.size() > 1) printf("Params 1: %s\n", params[1].get_str().c_str());

    static CReserveKey reservekey(pwalletMain);

    vector<unsigned char> blockData(ParseHex(params[0].get_str()));
    // FIXME blockData.insert( blockData.begin() + 80, 89, 0);
    CDataStream ssBlock(blockData, SER_NETWORK, PROTOCOL_VERSION);
    CBlock block;
    try {
        ssBlock >> block;
    }
    catch (std::exception &e) {
        printf("block decode failed.");
        throw JSONRPCError(-22, "Block decode failed");
    }

    /*
    printf("Block decode success.\n");
    CBlock * pblock = CreateNewBlock(pwalletMain); 
    block.fProofOfBurn = pblock->fProofOfBurn;
    block.burnHash = pblock->burnHash;
    block.burnBlkHeight = pblock->burnBlkHeight;
    block.burnCTx = pblock->burnCTx;
    block.burnCTxOut = pblock->burnCTxOut;
    block.nBurnBits = pblock->nBurnBits;
    block.nEffectiveBurnCoins = pblock->nEffectiveBurnCoins;
    delete pblock;
    printf("Block burn details update success.\n");
    */

    // PPCoin: sign block
    if (!block.SignBlock(*pwalletMain))
        throw JSONRPCError(-100, "Unable to sign block, wallet locked?");
    printf("Block sign success.\n"); 

    bool fAccepted = CheckWork(&block, *pwalletMain, *pMiningKey);
    printf("Checkwork success.\n");
    if (!fAccepted)
        return "rejected"; // TODO: report validation state

    return Value::null;
}

Value getmemorypool(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "getmemorypool [data]\n"
            "If [data] is not specified, returns data needed to construct a block to work on:\n"
            "  \"version\" : block version\n"
            "  \"previousblockhash\" : hash of current highest block\n"
            "  \"transactions\" : contents of non-coinbase transactions that should be included in the next block\n"
            "  \"coinbasevalue\" : maximum allowable input to coinbase transaction, including the generation award and transaction fees\n"
            "  \"coinbaseflags\" : data that should be included in coinbase so support for new features can be judged\n"
            "  \"time\" : timestamp appropriate for next block\n"
            "  \"mintime\" : minimum timestamp appropriate for next block\n"
            "  \"curtime\" : current timestamp\n"
            "  \"bits\" : compressed target of next block\n"
            "If [data] is specified, tries to solve the block and returns true if it was successful.");

    if (params.size() == 0)
    {
        if (vNodes.empty())
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "SLIMCoin is not connected!");

        if (IsInitialBlockDownload())
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "SLIMCoin is downloading blocks...");

        static CReserveKey reservekey(pwalletMain);

        // Update block
        static unsigned int nTransactionsUpdatedLast;
        static CBlockIndex* pindexPrev;
        static int64 nStart;
        static CBlock* pblock;
        if (pindexPrev != pindexBest ||
             (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 5))
        {
            nTransactionsUpdatedLast = nTransactionsUpdated;
            pindexPrev = pindexBest;
            nStart = GetTime();

            // Create new block
            if (pblock)
                delete pblock;
            pblock = CreateNewBlock(pwalletMain, false);
            if (!pblock)
                throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");
        }

        // Update nTime
        pblock->UpdateTime(pindexPrev);
        pblock->nNonce = 0;

        Array transactions;
        BOOST_FOREACH(CTransaction tx, pblock->vtx) {
            if (tx.IsCoinBase() || tx.IsCoinStake())
                continue;

            CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
            ssTx << tx;

            transactions.push_back(HexStr(ssTx.begin(), ssTx.end()));
        }

        Object result;
        result.push_back(Pair("version", pblock->nVersion));
        result.push_back(Pair("previousblockhash", pblock->hashPrevBlock.GetHex()));
        result.push_back(Pair("transactions", transactions));
        result.push_back(Pair("coinbasevalue", (int64_t)pblock->vtx[0].vout[0].nValue));
        result.push_back(Pair("coinbaseflags", HexStr(COINBASE_FLAGS.begin(), COINBASE_FLAGS.end())));
        result.push_back(Pair("time", (int64_t)pblock->nTime));
        result.push_back(Pair("mintime", (int64_t)pindexPrev->GetMedianTimePast()+1));
        result.push_back(Pair("curtime", (int64_t)GetAdjustedTime()));
        result.push_back(Pair("bits", HexBits(pblock->nBits)));

        return result;
    }
    else
    {
        // Parse parameters
        CDataStream ssBlock(ParseHex(params[0].get_str()), SER_NETWORK, PROTOCOL_VERSION);
        CBlock pblock;
        ssBlock >> pblock;

        static CReserveKey reservekey(pwalletMain);

        if (!pblock.SignBlock(*pwalletMain))
            throw JSONRPCError(RPC_UNABLE_TO_SIGN_BLOCK, "Unable to sign block, wallet locked?");
        return CheckWork(&pblock, *pwalletMain, reservekey);
    }
}

Value getblockhash(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "getblockhash <index>\n"
            "Returns hash of block in best-block-chain at <index>.");

    int nHeight = params[0].get_int();
    if (nHeight < 0 || nHeight > nBestHeight)
        throw runtime_error("Block number out of range.");

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hashBestChain];
    while (pblockindex->nHeight > nHeight)
        pblockindex = pblockindex->pprev;
    return pblockindex->phashBlock->GetHex();
}

Value getblock(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 3)
        throw runtime_error(
            "getblock <hash> [txinfo] [txdetails]\n"
            "txinfo optional to print more detailed tx info\n"
            "txdetails optional to print even more detailed tx info\n"
            "Returns details of a block with given block-hash.");

    std::string strHash = params[0].get_str();
    uint256 hash(strHash);

    if (mapBlockIndex.count(hash) == 0)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hash];
    block.ReadFromDisk(pblockindex, true, false);

    bool fTxInfo = params.size() > 1 ? params[1].get_bool() : false;
    bool fTxDetails = params.size() > 2 ? params[2].get_bool() : false;

    return blockToJSON(block, pblockindex, fTxInfo, fTxDetails);
}

// ppcoin: get information of sync-checkpoint
Value getcheckpoint(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getcheckpoint\n"
            "Show info of synchronized checkpoint.\n");

    Object result;
    CBlockIndex* pindexCheckpoint;
    
    result.push_back(Pair("synccheckpoint", Checkpoints::hashSyncCheckpoint.ToString().c_str()));
    pindexCheckpoint = mapBlockIndex[Checkpoints::hashSyncCheckpoint];        
    result.push_back(Pair("height", pindexCheckpoint->nHeight));
    result.push_back(Pair("timestamp", DateTimeStrFormat(pindexCheckpoint->GetBlockTime()).c_str()));
    if (mapArgs.count("-checkpointkey"))
        result.push_back(Pair("checkpointmaster", true));

    return result;
}

// ppcoin: reserve balance from being staked for network protection
Value reservebalance(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "reservebalance [<reserve> [amount]]\n"
            "<reserve> is true or false to turn balance reserve on or off.\n"
            "<amount> is a real and rounded to cent.\n"
            "Set reserve amount not participating in network protection.\n"
            "If no parameters provided current setting is printed.\n");

    if (params.size() > 0)
    {
        bool fReserve = params[0].get_bool();
        if (fReserve)
        {
            if (params.size() == 1)
                throw runtime_error("must provide amount to reserve balance.\n");
            int64 nAmount = AmountFromValue(params[1]);
            nAmount = (nAmount / CENT) * CENT;  // round to cent
            if (nAmount < 0)
                throw runtime_error("amount cannot be negative.\n");
            mapArgs["-reservebalance"] = FormatMoney(nAmount).c_str();
        }
        else
        {
            if (params.size() > 1)
                throw runtime_error("cannot specify amount to turn off reserve.\n");
            mapArgs["-reservebalance"] = "0";
        }
    }

    Object result;
    int64 nReserveBalance = 0;
    if (mapArgs.count("-reservebalance") && !ParseMoney(mapArgs["-reservebalance"], nReserveBalance))
        throw runtime_error("invalid reserve balance amount\n");
    result.push_back(Pair("reserve", (nReserveBalance > 0)));
    result.push_back(Pair("amount", ValueFromAmount(nReserveBalance)));
    return result;
}

// ppcoin: check wallet integrity
Value checkwallet(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "checkwallet\n"
            "Check wallet for integrity.\n");

    int nMismatchSpent;
    int64 nBalanceInQuestion;
    pwalletMain->FixSpentCoins(nMismatchSpent, nBalanceInQuestion, true);
    Object result;
    if (nMismatchSpent == 0)
        result.push_back(Pair("wallet check passed", true));
    else
    {
        result.push_back(Pair("mismatched spent coins", nMismatchSpent));
        result.push_back(Pair("amount in question", ValueFromAmount(nBalanceInQuestion)));
    }
    return result;
}

// ppcoin: repair wallet
Value repairwallet(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 0)
        throw runtime_error(
            "repairwallet\n"
            "Repair wallet if checkwallet reports any problem.\n");

    int nMismatchSpent;
    int64 nBalanceInQuestion;
    pwalletMain->FixSpentCoins(nMismatchSpent, nBalanceInQuestion);
    Object result;
    if (nMismatchSpent == 0)
        result.push_back(Pair("wallet check passed", true));
    else
    {
        result.push_back(Pair("mismatched spent coins", nMismatchSpent));
        result.push_back(Pair("amount affected by repair", ValueFromAmount(nBalanceInQuestion)));
    }
    return result;
}

// zapwallettxes
Value zapwallettxes(const Array& params, bool fHelp)
{
  if (fHelp || params.size() > 0)
    throw runtime_error("zapwallettxes\n"
          "Delete all wallet transactions and only recover those parts of the blockchain through -rescan on startup\n");

  std::vector<CWalletTx> vWtx;
  Object result;

  const char *mess="Zapping all transactions from wallet ...\n";
  printf("%s",mess); // to debug.log

  pwalletMain = new CWallet("wallet.dat");
  int nZapWalletRet = pwalletMain->ZapWalletTx(vWtx);
  if (nZapWalletRet != 0)
  {
    mess="Error loading wallet.dat: Wallet corrupted\n";
    printf("%s",mess);
    return(mess);
  }

  delete pwalletMain;
  pwalletMain = NULL;

  mess="Loading wallet...\n";
  printf("%s",mess);

  bool fFirstRun = true;
  pwalletMain = new CWallet("wallet.dat");


  int nLoadWalletRet = pwalletMain->LoadWallet(fFirstRun);
  if (nLoadWalletRet != 0)
  {
    if (nLoadWalletRet == 1)
    {
      mess="Error loading wallet.dat: Wallet corrupted\n";
      printf("%s",mess);
      return(mess);
    }
    else if (nLoadWalletRet == 2)
    {
      mess="Warning: error reading wallet.dat! All keys read correctly, but transaction data or address book entries might be missing or incorrect.\n";
      printf("%s",mess);
    }
    else if (nLoadWalletRet == 3)
    {
      mess="Error loading wallet.dat: Wallet requires newer version of Bitcoin-scrypt\n";
      printf("%s",mess);
      return(mess);
    }
    else if (nLoadWalletRet == 4)
    {
      mess="Wallet needed to be rewritten: restart Slimcoin to complete\n";
      printf("%s",mess);
      return(mess);
    }
    else
    {
      mess="Unknown error loading wallet.dat\n";
      printf("%s",mess);
      return(mess);
    } 
  }
  
  mess="Wallet loaded...\n";
  printf("%s",mess);

  mess="Loaded lables...\n";
  printf("%s",mess);

  // Restore wallet transaction metadata
  BOOST_FOREACH(const CWalletTx& wtxOld, vWtx)
  {
    uint256 hash = wtxOld.GetHash();
    std::map<uint256, CWalletTx>::iterator mi = pwalletMain->mapWallet.find(hash);
    if (mi != pwalletMain->mapWallet.end())
    {
      const CWalletTx* copyFrom = &wtxOld;
      CWalletTx* copyTo = &mi->second;
      copyTo->mapValue = copyFrom->mapValue;
      copyTo->vOrderForm = copyFrom->vOrderForm;
      copyTo->nTimeReceived = copyFrom->nTimeReceived;
      copyTo->nTimeSmart = copyFrom->nTimeSmart;
      copyTo->fFromMe = copyFrom->fFromMe;
      copyTo->strFromAccount = copyFrom->strFromAccount;
      copyTo->nOrderPos = copyFrom->nOrderPos;
      copyTo->WriteToDisk();
    }
  }
  mess="scanning for transactions...\n";
  printf("%s",mess);

  pwalletMain->ScanForWalletTransactions(pindexGenesisBlock, true);
  pwalletMain->ReacceptWalletTransactions();
  mess="Please restart your wallet.\n";
  printf("%s",mess);

  mess="Zap Wallet Finished.\nPlease restart your wallet for changes to take effect.\n";

  return (mess);
}

Value getsubsidy(const Array& params, bool fHelp)
{
    static CBlock* pblock;
    pblock = CreateNewBlock(pwalletMain, false);
    return (boost::int64_t)GetProofOfWorkReward(pblock->nBits);
}

// ppcoin: make a public-private key pair
Value makekeypair(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "makekeypair [prefix] [rounds]\n"
            "Make a public/private ECC key pair.\n"
            "[prefix] is optional preferred prefix for the public key.\n"
            "[rounds] is optional number of rounds of hashing, default 10000.\n");

    string strPrefix = "";
    int nRounds = 10000;

    if (params.size() > 0)
        strPrefix = params[0].get_str();
 
    if (params.size() > 1) {
        int rounds = params[1].get_int();
        if (rounds > nRounds)
            nRounds = rounds;
    }

    Object result;

    CKey key;
    int nCount = 0;
    CPubKey vchPubKey;
    CSecret secret;
    CPrivKey privkey;
    vector<unsigned char> sec;
    do
    {
        CKey testkey = CKey();
        testkey.MakeNewKey(true);
        privkey = testkey.GetPrivKey();

        secret.resize(32);
        memcpy(&secret[0], &privkey, 32);

        sec.resize(32);
        memcpy(&sec[0], &secret[0], 32);

        key.SetSecret(secret, true);
        vchPubKey = key.GetPubKey();

        nCount++;

    } while (nCount < nRounds && strPrefix != CBitcoinAddress(vchPubKey.GetID()).ToString().substr(0, strPrefix.size()));

    if (strPrefix != CBitcoinAddress(vchPubKey.GetID()).ToString().substr(0, strPrefix.size()))
        return Value::null;

    string fcompressed;
    for(int nCompressed=0; nCompressed<2; nCompressed++)
    {
      bool fCompressed = nCompressed == 1;
      if (fCompressed) {
        fcompressed = "compressed";
      }
      else {
        fcompressed = "uncompressed";
      }
      result.push_back(Pair("  * : ", fcompressed.c_str()));
      CBitcoinSecret bsecret;
      bsecret.SetSecret(secret, fCompressed);
      result.push_back(Pair("    * secret (base58): ", bsecret.ToString().c_str()));
      CKey key;
      key.SetSecret(secret, fCompressed);
      CPubKey vchPubKey = key.GetPubKey();
      vector<unsigned char> xvchPubKey = vchPubKey.Raw();
      result.push_back(Pair("    * pubkey (hex): ", HexStr(xvchPubKey.begin(), xvchPubKey.end()).c_str()));
      result.push_back(Pair("    * address (base58): ", CBitcoinAddress(vchPubKey.GetID()).ToString().c_str()));
    }

    return result;
}

Value dumpbootstrap(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 3)
        throw runtime_error(
            "dumpbootstrap <destination> <endblock> [startblock=0]\n"
            "Creates a bootstrap format block dump of the blockchain in destination, which can be a directory or a path with filename, up to the given endblock number.\n"
            "Optional <startblock> is the first block number to dump.");

    string strDest = params[0].get_str();
    int nEndBlock = params[1].get_int();
    if (nEndBlock < 0 || nEndBlock > nBestHeight)
        throw runtime_error("End block number out of range.");

    int nStartBlock = 0;
    if (params.size() > 2)
        nStartBlock = params[2].get_int();
    if (nStartBlock < 0 || nStartBlock > nEndBlock)
        throw runtime_error("Start block number out of range.");

    boost::filesystem::path pathDest(strDest);
    if (boost::filesystem::is_directory(pathDest))
        pathDest /= "bootstrap.dat";

    try {
        FILE* file = fopen(pathDest.string().c_str(), "wb");
        if (!file)
            throw JSONRPCError(-1, "Error: Could not open bootstrap file for writing.");

        CAutoFile fileout = CAutoFile(file, SER_DISK, CLIENT_VERSION);
        if (!fileout)
            throw JSONRPCError(-1, "Error: Could not open bootstrap file for writing.");

        unsigned char pchMessageStart[4];
        GetMessageStart(pchMessageStart, true);

        for (int nHeight = nStartBlock; nHeight <= nEndBlock; nHeight++)
        {
            CBlock block;
            CBlockIndex* pblockindex = FindBlockByHeight(nHeight);
            block.ReadFromDisk(pblockindex, true);
            fileout << FLATDATA(pchMessageStart) << fileout.GetSerializeSize(block) << block;
        }

    } catch(const boost::filesystem::filesystem_error &e) {
        throw JSONRPCError(-1, "Error: Bootstrap dump failed!");
    }

    return "bootstrap file created";
}

Value linearizehashes(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 3)
        throw runtime_error(
            "linearizehashes <destination> <endblock>  [startblock=0]\n"
            "Creates a dump of linearized block hashes in destination, which can be a directory or a path with filename, up to the given endblock number.\n"
            "Optional <startblock> is the first block number to dump.");

    string strDest = params[0].get_str();

    int nEndBlock = 1646900; // 3rd Feb 2019
    if (params.size() > 1)
        nEndBlock = params[1].get_int();
    if (nEndBlock < 0 || nEndBlock > nBestHeight)
        throw runtime_error("End block number out of range.");

    int nStartBlock = 0;
    if (params.size() > 2)
        nStartBlock = params[2].get_int();
    if (nStartBlock < 0 || nStartBlock > nEndBlock)
        throw runtime_error("Start block number out of range.");

    boost::filesystem::path pathDest(strDest);
    if (boost::filesystem::is_directory(pathDest))
        pathDest /= "hashlist.txt";

    try {
        FILE* file = fopen(pathDest.string().c_str(), "w");
        if (!file)
            throw JSONRPCError(-1, "Error: Could not open output file for writing.");

        CAutoFile fileout = CAutoFile(file, SER_DISK, CLIENT_VERSION);
        if (!fileout)
            throw JSONRPCError(-1, "Error: Could not open output file for writing.");

        for (int nHeight = nStartBlock; nHeight <= nEndBlock; nHeight++)
        {
            CBlock block;
            CBlockIndex* pblockindex = FindBlockByHeight(nHeight);
            block.ReadFromDisk(pblockindex, true);
            std::string blockhash = block.GetHash().ToString().c_str();
            fileout << blockhash.append("\n");
        }
    } catch(const boost::filesystem::filesystem_error &e) {
        throw JSONRPCError(-1, "Error: Linearized hash dump failed!");
    }

    return "file of linearized hashes created";
}

extern CCriticalSection cs_mapAlerts;
extern map<uint256, CAlert> mapAlerts;

// ppcoin: send alert.  
// There is a known deadlock situation with ThreadMessageHandler
// ThreadMessageHandler: holds cs_vSend and acquiring cs_main in SendMessages()
// ThreadRPCServer: holds cs_main and acquiring cs_vSend in alert.RelayTo()/PushMessage()/BeginMessage()
Value sendalert(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 6)
        throw runtime_error(
            "sendalert <message> <privatekey> <minver> <maxver> <priority> <id> [cancelupto]\n"
            "<message> is the alert text message\n"
            "<privatekey> is hex string of alert master private key\n"
            "<minver> is the minimum applicable internal client version\n"
            "<maxver> is the maximum applicable internal client version\n"
            "<priority> is integer priority number\n"
            "<id> is the alert id\n"
            "[cancelupto] cancels all alert id's up to this number\n"
            "Returns true or false.");

    CAlert alert;
    CKey key;

    alert.strStatusBar = params[0].get_str();
    alert.nMinVer = params[2].get_int();
    alert.nMaxVer = params[3].get_int();
    alert.nPriority = params[4].get_int();
    alert.nID = params[5].get_int();
    if (params.size() > 6)
        alert.nCancel = params[6].get_int();
    alert.nVersion = PROTOCOL_VERSION;
    alert.nRelayUntil = GetAdjustedTime() + 365*24*60*60;
    alert.nExpiration = GetAdjustedTime() + 365*24*60*60;
    alert.nPriority = 1;

    CDataStream sMsg(SER_NETWORK, PROTOCOL_VERSION);
    sMsg << (CUnsignedAlert)alert;
    alert.vchMsg = vector<unsigned char>(sMsg.begin(), sMsg.end());
    
    vector<unsigned char> vchPrivKey = ParseHex(params[1].get_str());
    key.SetPrivKey(CPrivKey(vchPrivKey.begin(), vchPrivKey.end())); // if key is not correct openssl may crash
    if (!key.Sign(Hash(alert.vchMsg.begin(), alert.vchMsg.end()), alert.vchSig))
        throw runtime_error(
            "Unable to sign alert, check private key?\n");  
    if(!alert.ProcessAlert()) 
        throw runtime_error(
            "Failed to process alert.\n");
    // Relay alert
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            alert.RelayTo(pnode);
    }

    Object result;
    result.push_back(Pair("strStatusBar", alert.strStatusBar));
    result.push_back(Pair("nVersion", alert.nVersion));
    result.push_back(Pair("nMinVer", alert.nMinVer));
    result.push_back(Pair("nMaxVer", alert.nMaxVer));
    result.push_back(Pair("nPriority", alert.nPriority));
    result.push_back(Pair("nID", alert.nID));
    if (alert.nCancel > 0)
        result.push_back(Pair("nCancel", alert.nCancel));
    return result;
}

Value getrawmempool(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 0)
        throw runtime_error(
            "getrawmempool\n"
            "Returns all transaction ids in memory pool.");

    vector<uint256> vtxid;
    mempool.queryHashes(vtxid);

    Array a;
    BOOST_FOREACH(const uint256& hash, vtxid)
        a.push_back(hash.ToString());

    return a;
}

//
// Raw transactions
//
Value getrawtransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getrawtransaction <txid> [verbose=0]\n"
            "If verbose=0, returns a string that is\n"
            "serialized, hex-encoded data for <txid>.\n"
            "If verbose is non-zero, returns an Object\n"
            "with information about <txid>.");

    uint256 hash;
    hash.SetHex(params[0].get_str());

    bool fVerbose = false;
    if (params.size() > 1)
        fVerbose = (params[1].get_int() != 0);

    CTransaction tx;
    uint256 hashBlock = 0;
    if (!GetTransaction(hash, tx, hashBlock))
        throw JSONRPCError(-5, "No information available about transaction");

    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << tx;
    string strHex = HexStr(ssTx.begin(), ssTx.end());

    if (!fVerbose)
        return strHex;

    Object result;
    result.push_back(Pair("hex", strHex));
    TxToJSON(tx, hashBlock, result);
    return result;
}

Value listunspent(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 3)
        throw runtime_error(
            "listunspent [minconf=1] [maxconf=9999999]  [\"address\",...]\n"
            "Returns array of unspent transaction outputs\n"
            "with between minconf and maxconf (inclusive) confirmations.\n"
            "Optionally filtered to only include txouts paid to specified addresses.\n"
            "Results are an array of Objects, each of which has:\n"
            "{txid, vout, scriptPubKey, amount, confirmations}");

    int nMinDepth = 1;
    if (params.size() > 0)
        nMinDepth = params[0].get_int();

    int nMaxDepth = 9999999;
    if (params.size() > 1)
        nMaxDepth = params[1].get_int();

    set<CBitcoinAddress> setAddress;
    if (params.size() > 2)
    {
        Array inputs = params[2].get_array();
        BOOST_FOREACH(Value& input, inputs)
        {
            CBitcoinAddress address(input.get_str());
            if (!address.IsValid())
                throw JSONRPCError(-5, string("Invalid Bitcoin address: ")+input.get_str());
            if (setAddress.count(address))
                throw JSONRPCError(-8, string("Invalid parameter, duplicated address: ")+input.get_str());
           setAddress.insert(address);
        }
    }

    Array results;
    vector<COutput> vecOutputs;
    pwalletMain->AvailableCoins((unsigned int)GetAdjustedTime(), vecOutputs, false);
    BOOST_FOREACH(const COutput& out, vecOutputs)
    {
        if (out.nDepth < nMinDepth || out.nDepth > nMaxDepth)
            continue;

        if(setAddress.size())
        {
            CTxDestination address;
            if(!ExtractDestination(out.tx->vout[out.i].scriptPubKey, address))
                continue;

            if (!setAddress.count(address))
                continue;
        }

        int64 nValue = out.tx->vout[out.i].nValue;
        const CScript& pk = out.tx->vout[out.i].scriptPubKey;
        CTxDestination address;
        Object entry;
        entry.push_back(Pair("txid", out.tx->GetHash().GetHex()));
        entry.push_back(Pair("vout", out.i));
        if (ExtractDestination(pk, address))
        {
            entry.push_back(Pair("address", CBitcoinAddress(address).ToString()));
            if (pwalletMain->mapAddressBook.count(address))
                entry.push_back(Pair("account", pwalletMain->mapAddressBook[address]));
        }
        entry.push_back(Pair("scriptPubKey", HexStr(pk.begin(), pk.end())));
        entry.push_back(Pair("amount",ValueFromAmount(nValue)));
        entry.push_back(Pair("confirmations",out.nDepth));
        results.push_back(entry);
    }

    return results;
}

Value createrawtransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 2)
        throw runtime_error(
            "createrawtransaction [{\"txid\":txid,\"vout\":n},...] {address:amount,...}\n"
            "Create a transaction spending given inputs\n"
            "(array of objects containing transaction id and output number),\n"
            "sending to given address(es).\n"
            "Returns hex-encoded raw transaction.\n"
            "Note that the transaction's inputs are not signed, and\n"
            "it is not stored in the wallet or transmitted to the network.");

    Array inputs = params[0].get_array();
    Object sendTo = params[1].get_obj();

    CTransaction rawTx;

    BOOST_FOREACH(Value& input, inputs)
    {
        const Object& o = input.get_obj();

        const Value& txid_v = find_value(o, "txid");
        if (txid_v.type() != str_type)
            throw JSONRPCError(-8, "Invalid parameter, missing txid key");
        string txid = txid_v.get_str();
        if (!IsHex(txid))
            throw JSONRPCError(-8, "Invalid parameter, expected hex txid");

        const Value& vout_v = find_value(o, "vout");
        if (vout_v.type() != int_type)
            throw JSONRPCError(-8, "Invalid parameter, missing vout key");
        int nOutput = vout_v.get_int();
        if (nOutput < 0)
            throw JSONRPCError(-8, "Invalid parameter, vout must be positive");

        CTxIn in(COutPoint(uint256(txid), nOutput));
        rawTx.vin.push_back(in);
    }

    set<CBitcoinAddress> setAddress;
    BOOST_FOREACH(const Pair& s, sendTo)
    {
        CBitcoinAddress address(s.name_);
        if (!address.IsValid())
            throw JSONRPCError(-5, string("Invalid Bitcoin address: ")+s.name_);

        if (setAddress.count(address))
            throw JSONRPCError(-8, string("Invalid parameter, duplicated address: ")+s.name_);
        setAddress.insert(address);

        CScript scriptPubKey;
        scriptPubKey.SetDestination(address.Get());
        int64 nAmount = AmountFromValue(s.value_);

        CTxOut out(nAmount, scriptPubKey);
        rawTx.vout.push_back(out);
    }

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << rawTx;
    return HexStr(ss.begin(), ss.end());
}

Value decoderawtransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "decoderawtransaction <hex string>\n"
            "Return a JSON object representing the serialized, hex-encoded transaction.");

    vector<unsigned char> txData(ParseHex(params[0].get_str()));
    CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);
    CTransaction tx;
    try {
        ssData >> tx;
    }
    catch (std::exception &e) {
        throw JSONRPCError(-22, "TX decode failed");
    }

    Object result;
    TxToJSON(tx, 0, result);

    return result;
}

Value signrawtransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 4)
        throw runtime_error(
            "signrawtransaction <hex string> [{\"txid\":txid,\"vout\":n,\"scriptPubKey\":hex},...] [<privatekey1>,...] [sighashtype=\"ALL\"]\n"
            "Sign inputs for raw transaction (serialized, hex-encoded).\n"
            "Second optional argument (may be null) is an array of previous transaction outputs that\n"
            "this transaction depends on but may not yet be in the blockchain.\n"
            "Third optional argument (may be null) is an array of base58-encoded private\n"
            "keys that, if given, will be the only keys used to sign the transaction.\n"
            "Fourth optional argument is a string that is one of six values; ALL, NONE, SINGLE or\n"
            "ALL|ANYONECANPAY, NONE|ANYONECANPAY, SINGLE|ANYONECANPAY.\n"
            "Returns json object with keys:\n"
            "  hex : raw transaction with signature(s) (hex-encoded string)\n"
            "  complete : 1 if transaction has a complete set of signature (0 if not)\n");

    vector<unsigned char> txData(ParseHex(params[0].get_str()));
    CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);
    vector<CTransaction> txVariants;
    while (!ssData.empty())
    {
        try {
            CTransaction tx;
            ssData >> tx;
            txVariants.push_back(tx);
        }
        catch (std::exception &e) {
            throw JSONRPCError(-22, "TX decode failed");
        }
    }

    if (txVariants.empty())
        throw JSONRPCError(-22, "Missing transaction");

    // mergedTx will end up with all the signatures; it
    // starts as a clone of the rawtx:
    CTransaction mergedTx(txVariants[0]);
    bool fComplete = true;

    // Fetch previous transactions (inputs):
    map<COutPoint, CScript> mapPrevOut;
    for (unsigned int i = 0; i < mergedTx.vin.size(); i++)
    {
        CTransaction tempTx;
        MapPrevTx mapPrevTx;
        CTxDB txdb("r");
        map<uint256, CTxIndex> unused;
        bool fInvalid;

        // FetchInputs aborts on failure, so we go one at a time.
        tempTx.vin.push_back(mergedTx.vin[i]);
        tempTx.FetchInputs(txdb, unused, false, false, mapPrevTx, fInvalid);

        // Copy results into mapPrevOut:
        BOOST_FOREACH(const CTxIn& txin, tempTx.vin)
        {
            const uint256& prevHash = txin.prevout.hash;
            if (mapPrevTx.count(prevHash) && mapPrevTx[prevHash].second.vout.size()>txin.prevout.n)
                mapPrevOut[txin.prevout] = mapPrevTx[prevHash].second.vout[txin.prevout.n].scriptPubKey;
        }
    }

    bool fGivenKeys = false;
    CBasicKeyStore tempKeystore;
    if (params.size() > 2 && params[2].type() != null_type)
    {
        fGivenKeys = true;
        Array keys = params[2].get_array();
        BOOST_FOREACH(Value k, keys)
        {
            CBitcoinSecret vchSecret;
            bool fGood = vchSecret.SetString(k.get_str());
            if (!fGood)
                throw JSONRPCError(-5,"Invalid private key");
            CKey key;
            bool fCompressed;
            CSecret secret = vchSecret.GetSecret(fCompressed);
            key.SetSecret(secret, fCompressed);
            tempKeystore.AddKey(key);
        }
    }
    else if(fWalletUnlockMintOnly)
        throw JSONRPCError(-102, "Wallet is unlocked for minting only.");
    else if(pwalletMain->IsLocked())
        throw JSONRPCError(-13, "The wallet must be unlocked with walletpassphrase first");

    // Add previous txouts given in the RPC call:
    if (params.size() > 1 && params[1].type() != null_type)
    {
        Array prevTxs = params[1].get_array();
        BOOST_FOREACH(Value& p, prevTxs)
        {
            if (p.type() != obj_type)
                throw JSONRPCError(-22, "expected object with {\"txid'\",\"vout\",\"scriptPubKey\"}");

            Object prevOut = p.get_obj();

            string txidHex = find_value(prevOut, "txid").get_str();
            if (!IsHex(txidHex))
                throw JSONRPCError(-22, "txid must be hexadecimal");
            uint256 txid;
            txid.SetHex(txidHex);

            int nOut = find_value(prevOut, "vout").get_int();
            if (nOut < 0)
                throw JSONRPCError(-22, "vout must be positive");

            string pkHex = find_value(prevOut, "scriptPubKey").get_str();
            if (!IsHex(pkHex))
                throw JSONRPCError(-22, "scriptPubKey must be hexadecimal");
            vector<unsigned char> pkData(ParseHex(pkHex));
            CScript scriptPubKey(pkData.begin(), pkData.end());

            COutPoint outpoint(txid, nOut);
            if (mapPrevOut.count(outpoint))
            {
                // Complain if scriptPubKey doesn't match
                if (mapPrevOut[outpoint] != scriptPubKey)
                {
                    string err("Previous output scriptPubKey mismatch:\n");
                    err = err + mapPrevOut[outpoint].ToString() + "\nvs:\n"+
                        scriptPubKey.ToString();
                    throw JSONRPCError(-22, err);
                }
            }
            else
                mapPrevOut[outpoint] = scriptPubKey;

            // if redeemScript given and not using the local wallet (private keys
            // given), add redeemScript to the tempKeystore so it can be signed:
            if (fGivenKeys && scriptPubKey.IsPayToScriptHash())
            {
                Value v = find_value(prevOut, "redeemScript");
                if (!(v == Value::null))
                {
                    vector<unsigned char> rsData(ParseHex(v.get_str()));
                    CScript redeemScript(rsData.begin(), rsData.end());
                    tempKeystore.AddCScript(redeemScript);
                }
            }
        }
    }

    const CKeyStore& keystore = (fGivenKeys ? tempKeystore : *pwalletMain);

    int nHashType = SIGHASH_ALL;
    if (params.size() > 3 && params[3].type() != null_type)
    {
        static map<string, int> mapSigHashValues =
            boost::assign::map_list_of
            (string("ALL"), int(SIGHASH_ALL))
            (string("ALL|ANYONECANPAY"), int(SIGHASH_ALL|SIGHASH_ANYONECANPAY))
            (string("NONE"), int(SIGHASH_NONE))
            (string("NONE|ANYONECANPAY"), int(SIGHASH_NONE|SIGHASH_ANYONECANPAY))
            (string("SINGLE"), int(SIGHASH_SINGLE))
            (string("SINGLE|ANYONECANPAY"), int(SIGHASH_SINGLE|SIGHASH_ANYONECANPAY))
            ;
        string strHashType = params[3].get_str();
        if (mapSigHashValues.count(strHashType))
            nHashType = mapSigHashValues[strHashType];
        else
            throw JSONRPCError(-8, "Invalid sighash param");
    }

    bool fHashSingle = ((nHashType & ~SIGHASH_ANYONECANPAY) == SIGHASH_SINGLE);

    // Sign what we can:
    for (unsigned int i = 0; i < mergedTx.vin.size(); i++)
    {
        CTxIn& txin = mergedTx.vin[i];
        if (mapPrevOut.count(txin.prevout) == 0)
        {
            fComplete = false;
            continue;
        }
        const CScript& prevPubKey = mapPrevOut[txin.prevout];

        txin.scriptSig.clear();
        // Only sign SIGHASH_SINGLE if there's a corresponding output:
        if (!fHashSingle || (i < mergedTx.vout.size()))
            SignSignature(keystore, prevPubKey, mergedTx, i, nHashType);

        // ... and merge in other signatures:
        BOOST_FOREACH(const CTransaction& txv, txVariants)
        {
            txin.scriptSig = CombineSignatures(prevPubKey, mergedTx, i, txin.scriptSig, txv.vin[i].scriptSig);
        }
        if (!VerifyScript(txin.scriptSig, prevPubKey, mergedTx, i, true, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC | SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY, 0, nullptr))
            fComplete = false;
    }

    Object result;
    CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
    ssTx << mergedTx;
    result.push_back(Pair("hex", HexStr(ssTx.begin(), ssTx.end())));
    result.push_back(Pair("complete", fComplete));

    return result;
}

Value sendrawtransaction(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "sendrawtransaction <hex string> [checkinputs=0]\n"
            "Submits raw transaction (serialized, hex-encoded) to local node and network.\n"
            "If checkinputs is non-zero, checks the validity of the inputs of the transaction before sending it.");

    // parse hex string from parameter
    vector<unsigned char> txData(ParseHex(params[0].get_str()));
    CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);
    bool fCheckInputs = false;
    if (params.size() > 1)
        fCheckInputs = (params[1].get_int() != 0);
    CTransaction tx;

    // deserialize binary data stream
    try {
        ssData >> tx;
    }
    catch (std::exception &e) {
        throw JSONRPCError(-22, "TX decode failed");
    }
    uint256 hashTx = tx.GetHash();

    // See if the transaction is already in a block
    // or in the memory pool:
    CTransaction existingTx;
    uint256 hashBlock = 0;
    if (GetTransaction(hashTx, existingTx, hashBlock))
    {
        if (hashBlock != 0)
            throw JSONRPCError(-5, string("transaction already in block ")+hashBlock.GetHex());
        // Not in block, but already in the memory pool; will drop
        // through to re-relay it.
    }
    else
    {
        // push to local node
        CTxDB txdb("r");
        if (!tx.AcceptToMemoryPool(txdb, fCheckInputs))
            throw JSONRPCError(-22, "TX rejected");

        SyncWithWallets(tx, NULL, true);
    }
    RelayMessage(CInv(MSG_TX, hashTx), tx);

    return hashTx.GetHex();
}

Value getinscription(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1)
        throw runtime_error("getinscription <txid> \n");
    uint256 hash;
    hash.SetHex(params[0].get_str());

    CTransaction tx;
    uint256 hashBlock = 0;
    if (!GetTransaction(hash, tx, hashBlock))
        return false;

    try{
        std::string inscriptionStr;
        for (unsigned int i = 0; i < tx.vout.size(); i++)
        {
            const CTxOut& txout = tx.vout[i];
            const CScript& scriptPubKey = txout.scriptPubKey;
            txnouttype type;
            vector<valtype> vSolutions;
            if (Solver(scriptPubKey, type, vSolutions))
            {
                if (type == TX_NULL_DATA)
                {
                    char start = 2;
                    if ( txout.scriptPubKey[1] == 0x4c )
                        start = 3;

                    if ( txout.scriptPubKey[start] != 0xfa || txout.scriptPubKey[start + 1] != 0xce )
                        return false;
                    std::string str(txout.scriptPubKey.begin() + start + 4, txout.scriptPubKey.end());
                    Value valReply;
                    if (read_string("{\"value\":\"" + str + "\"}\"", valReply))
                    {
                        return valReply;
                    }
                }
            }
        }
    }catch(std::exception &e){
       return false;
    }
    return false;
}

Value getmoneysupply(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1)
        throw runtime_error("getmoneysupply <blockhash> \n");

    std::string strHash = params[0].get_str();
    uint256 hash(strHash);

    if (mapBlockIndex.count(hash) == 0)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hash];
    block.ReadFromDisk(pblockindex, true, false);

    Object result;
    result.push_back(Pair("moneysupply", ValueFromAmount(pblockindex->nMoneySupply)));
    return result;
}

Value getburnedcoins(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1)
        throw runtime_error("getburnedcoins <blockhash> \n");

    std::string strHash = params[0].get_str();
    uint256 hash(strHash);

    if (mapBlockIndex.count(hash) == 0)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hash];
    block.ReadFromDisk(pblockindex, true, false);

    Object result;
    result.push_back(Pair("burnedcoins", ValueFromAmount(pblockindex->burnt)));
    return result;
}


/*
Value getinscription(const Array& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 1)
        throw runtime_error("getinscription <txid> \n");
    uint256 hash;
    hash.SetHex(params[0].get_str());

    CTransaction tx;
    uint256 hashBlock = 0;
    if (!GetTransaction(hash, tx, hashBlock))
        return false;
    try{
        std::string inscriptionStr;
        for (unsigned int i = 0; i < tx.vout.size(); i++)
        {
            const CTxOut& txout = tx.vout[i];
            const CScript& scriptPubKey = txout.scriptPubKey;
            txnouttype type;
            vector<vector<unsigned char> > vSolutions;
            if (Solver(scriptPubKey, type, vSolutions))
            {
                if (type == TX_MULTISIG)
                {
                    std::string  asmStr  = scriptPubKey.ToString();
                    std::vector<std::string> strs;
                    boost::split(strs, asmStr, boost::is_any_of(" "));
                    std::string t = strs[2];
                    std::string s;
                    s.assign(t,2,2);
                    int x;
                    sscanf(s.c_str(), "%x", &x); 
                    t.assign(t,4,x*2); 
                    inscriptionStr.append(t);
                }
            }
        }

        int len = inscriptionStr.length();
        std::string strReply;
        for(int i=0; i< len; i+=2)
        {
            std::string byte = inscriptionStr.substr(i,2);
            char chr = (char) (int)strtol(byte.c_str(), NULL, 16);
            strReply.push_back(chr);
        }

        strReply = DecodeBase64(strReply);
        Value valReply;
        if (read_string(strReply, valReply))
        {
            return valReply;
        }
    }catch(std::exception &e){
       return false;
    }
    return false;
}
*/


//
// Call Table
//


static const CRPCCommand vRPCCommands[] =
{ //  name                      function                 safe mode?
    //  ------------------------  -----------------------  ----------
    { "help",                     &help,                   true   },
    { "stop",                     &stop,                   true   },
    { "calcburnhash",             &calcburnhash,           true   },
    { "burncoins",                &burncoins,              false  },
    { "getblockcount",            &getblockcount,          true   },
    { "getblocknumber",           &getblocknumber,         true   },
    { "getburndata",              &getburndata,            true   },
    { "getconnectioncount",       &getconnectioncount,     true   },
    { "getpeerinfo",              &getpeerinfo,            true   },
    { "addnode",                  &addnode,                true   },
    { "getdifficulty",            &getdifficulty,          true   },
    { "getgenerate",              &getgenerate,            true   },
    { "setgenerate",              &setgenerate,            true   },
    { "gethashespersec",          &gethashespersec,        true   },
    { "getnetworkghps",           &getnetworkghps,         true   },
    { "getinfo",                  &getinfo,                true   },
    { "getmininginfo",            &getmininginfo,          true   },
    { "getnewaddress",            &getnewaddress,          true   },
    { "getaccountaddress",        &getaccountaddress,      true   },
    { "setaccount",               &setaccount,             true   },
    { "getaccount",               &getaccount,             false  },
    { "getaddressesbyaccount",    &getaddressesbyaccount,  true   },
    { "sendtoaddress",            &sendtoaddress,          false  },
    { "getreceivedbyaddress",     &getreceivedbyaddress,   false  },
    { "getreceivedbyaccount",     &getreceivedbyaccount,   false  },
    { "listreceivedbyaddress",    &listreceivedbyaddress,  false  },
    { "listreceivedbyaccount",    &listreceivedbyaccount,  false  },
    { "backupwallet",             &backupwallet,           true   },
    { "keypoolrefill",            &keypoolrefill,          true   },
    { "walletpassphrase",         &walletpassphrase,       true   },
    { "walletpassphrasechange",   &walletpassphrasechange, false  },
    { "walletlock",               &walletlock,             true   },
    { "encryptwallet",            &encryptwallet,          false  },
    { "validateaddress",          &validateaddress,        true   },
    { "getbalance",               &getbalance,             false  },
    { "move",                     &movecmd,                false  },
    { "sendfrom",                 &sendfrom,               false  },
    { "sendmany",                 &sendmany,               false  },
    { "addmultisigaddress",       &addmultisigaddress,     false  },
    { "getblock",                 &getblock,               false  },
    { "getblockhash",             &getblockhash,           false  },
    { "gettransaction",           &gettransaction,         false  },
    { "listtransactions",         &listtransactions,       false  },
    { "listburnminted",           &listburnminted,         false  },
    { "signmessage",              &signmessage,            false  },
    { "verifymessage",            &verifymessage,          false  },
    { "getwork",                  &getwork,                true   },
    { "listaccounts",             &listaccounts,           false  },
    { "settxfee",                 &settxfee,               false  },
    { "getblocktemplate",         &getblocktemplate,       true   },
    { "submitblock",              &submitblock,            false  },
    { "listsinceblock",           &listsinceblock,         false  },
    { "dumpprivkey",              &dumpprivkey,            false  },
    { "importprivkey",            &importprivkey,          false  },
    { "importpassphrase",         &importpassphrase,       false  },
    { "getcheckpoint",            &getcheckpoint,          true   },
    { "reservebalance",           &reservebalance,         false  },
    { "dumpbootstrap",            &dumpbootstrap,          false  },
    { "linearizehashes",          &linearizehashes,        false  },
    { "makekeypair",              &makekeypair,            false  },
    { "encryptmessage",           &encryptmessage,         false  },
    { "decryptmessage",           &decryptmessage,         false  },
    { "encryptdata",              &encryptdata,            false  },
    { "decryptdata",              &decryptdata,            false  },
    { "sendalert",                &sendalert,              false  },
    { "listunspent",              &listunspent,            false  },
    { "getrawtransaction",        &getrawtransaction,      false  },
    { "createrawtransaction",     &createrawtransaction,   false  },
    { "decoderawtransaction",     &decoderawtransaction,   false  },
    { "signrawtransaction",       &signrawtransaction,     false  },
    { "sendrawtransaction",       &sendrawtransaction,     false  },
    { "getmemorypool",            &getmemorypool,          true   },
    { "getrawmempool",            &getrawmempool,          true   },
    { "getsubsidy",               &getsubsidy,             false  },
    { "getinscription",           &getinscription,         true   },
    { "getmoneysupply",           &getmoneysupply,         true   },
    { "getburnedcoins",           &getburnedcoins,         true   },
    { "checkwallet",              &checkwallet,            false  },
    { "repairwallet",             &repairwallet,           false  },
    { "zapwallettxes",            &zapwallettxes,          false  }};

CRPCTable::CRPCTable()
{
    unsigned int vcidx;
    for (vcidx = 0; vcidx < (sizeof(vRPCCommands) / sizeof(vRPCCommands[0])); vcidx++)
    {
        const CRPCCommand *pcmd;

        pcmd = &vRPCCommands[vcidx];
        mapCommands[pcmd->name] = pcmd;
    }
}

const CRPCCommand *CRPCTable::operator[](string name) const
{
    map<string, const CRPCCommand*>::const_iterator it = mapCommands.find(name);
    if (it == mapCommands.end())
        return NULL;
    return (*it).second;
}

//
// HTTP protocol
//
// This ain't Apache.  We're just using HTTP header for the length field
// and to be compatible with other JSON-RPC implementations.
//

string HTTPPost(const string& strMsg, const map<string,string>& mapRequestHeaders)
{
    ostringstream s;
    s << "POST / HTTP/1.1\r\n"
      << "User-Agent: slimcoin-json-rpc/" << FormatFullVersion() << "\r\n"
      << "Host: 127.0.0.1\r\n"
      << "Content-Type: application/json\r\n"
      << "Content-Length: " << strMsg.size() << "\r\n"
      << "Connection: close\r\n"
      << "Accept: application/json\r\n";
    BOOST_FOREACH(const PAIRTYPE(string, string)& item, mapRequestHeaders)
        s << item.first << ": " << item.second << "\r\n";
    s << "\r\n" << strMsg;

    return s.str();
}

string rfc1123Time()
{
    char buffer[64];
    time_t now;
    time(&now);
    struct tm* now_gmt = gmtime(&now);
    string locale(setlocale(LC_TIME, NULL));
    setlocale(LC_TIME, "C"); // we want posix (aka "C") weekday/month strings
    strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S +0000", now_gmt);
    setlocale(LC_TIME, locale.c_str());
    return string(buffer);
}

static string HTTPReply(int nStatus, const string& strMsg)
{
    if (nStatus == 401)
        return strprintf("HTTP/1.0 401 Authorization Required\r\n"
            "Date: %s\r\n"
            "Server: slimcoin-json-rpc/%s\r\n"
            "WWW-Authenticate: Basic realm=\"jsonrpc\"\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 296\r\n"
            "\r\n"
            "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\"\r\n"
            "\"http://www.w3.org/TR/1999/REC-html401-19991224/loose.dtd\">\r\n"
            "<HTML>\r\n"
            "<HEAD>\r\n"
            "<TITLE>Error</TITLE>\r\n"
            "<META HTTP-EQUIV='Content-Type' CONTENT='text/html; charset=ISO-8859-1'>\r\n"
            "</HEAD>\r\n"
            "<BODY><H1>401 Unauthorized.</H1></BODY>\r\n"
            "</HTML>\r\n", rfc1123Time().c_str(), FormatFullVersion().c_str());
    const char *cStatus;
    if (nStatus == 200) cStatus = "OK";
    else if (nStatus == 400) cStatus = "Bad Request";
    else if (nStatus == 403) cStatus = "Forbidden";
    else if (nStatus == 404) cStatus = "Not Found";
    else if (nStatus == 500) cStatus = "Internal Server Error";
    else cStatus = "";
    return strprintf(
            "HTTP/1.1 %d %s\r\n"
            "Date: %s\r\n"
            "Connection: close\r\n"
            "Content-Length: %d\r\n"
            "Content-Type: application/json\r\n"
            "Server: slimcoin-json-rpc/%s\r\n"
            "\r\n"
            "%s",
        nStatus,
        cStatus,
        rfc1123Time().c_str(),
        strMsg.size(),
        FormatFullVersion().c_str(),
        strMsg.c_str());
}

int ReadHTTPStatus(std::basic_istream<char>& stream)
{
    string str;
    getline(stream, str);
    vector<string> vWords;
    boost::split(vWords, str, boost::is_any_of(" "));
    if (vWords.size() < 2)
        return 500;
    return atoi(vWords[1].c_str());
}

int ReadHTTPHeader(std::basic_istream<char>& stream, map<string, string>& mapHeadersRet)
{
    int nLen = 0;
    while (true)
    {
        string str;
        std::getline(stream, str);
        if (str.empty() || str == "\r")
            break;
        string::size_type nColon = str.find(":");
        if (nColon != string::npos)
        {
            string strHeader = str.substr(0, nColon);
            boost::trim(strHeader);
            boost::to_lower(strHeader);
            string strValue = str.substr(nColon+1);
            boost::trim(strValue);
            mapHeadersRet[strHeader] = strValue;
            if (strHeader == "content-length")
                nLen = atoi(strValue.c_str());
        }
    }
    return nLen;
}

int ReadHTTP(std::basic_istream<char>& stream, map<string, string>& mapHeadersRet, string& strMessageRet)
{
    mapHeadersRet.clear();
    strMessageRet = "";

    // Read status
    int nStatus = ReadHTTPStatus(stream);

    // Read header
    int nLen = ReadHTTPHeader(stream, mapHeadersRet);
    if (nLen < 0 || nLen > (int)MAX_SIZE)
        return 500;

    // Read message
    if (nLen > 0)
    {
        vector<char> vch(nLen);
        stream.read(&vch[0], nLen);
        strMessageRet = string(vch.begin(), vch.end());
    }

    return nStatus;
}

bool HTTPAuthorized(map<string, string>& mapHeaders)
{
    string strAuth = mapHeaders["authorization"];
    if (strAuth.substr(0,6) != "Basic ")
        return false;
    string strUserPass64 = strAuth.substr(6); boost::trim(strUserPass64);
    string strUserPass = DecodeBase64(strUserPass64);
    return strUserPass == strRPCUserColonPass;
}

//
// JSON-RPC protocol.  Bitcoin speaks version 1.0 for maximum compatibility,
// but uses JSON-RPC 1.1/2.0 standards for parts of the 1.0 standard that were
// unspecified (HTTP errors and contents of 'error').
//
// 1.0 spec: http://json-rpc.org/wiki/specification
// 1.2 spec: http://groups.google.com/group/json-rpc/web/json-rpc-over-http
// http://www.codeproject.com/KB/recipes/JSON_Spirit.aspx
//

string JSONRPCRequest(const string& strMethod, const Array& params, const Value& id)
{
    Object request;
    request.push_back(Pair("method", strMethod));
    request.push_back(Pair("params", params));
    request.push_back(Pair("id", id));
    return write_string(Value(request), false) + "\n";
}

Object JSONRPCReplyObj(const Value& result, const Value& error, const Value& id)
{
    Object reply;
    if (error.type() != null_type)
        reply.push_back(Pair("result", Value::null));
    else
        reply.push_back(Pair("result", result));
    reply.push_back(Pair("error", error));
    reply.push_back(Pair("id", id));
    return reply;
}

string JSONRPCReply(const Value& result, const Value& error, const Value& id)
{
    Object reply = JSONRPCReplyObj(result, error, id);
    return write_string(Value(reply), false) + "\n";
}

void ErrorReply(std::ostream& stream, const Object& objError, const Value& id)
{
    // Send error reply from json-rpc error object
    int nStatus = 500;
    int code = find_value(objError, "code").get_int();
    if (code == -32600) nStatus = 400;
    else if (code == -32601) nStatus = 404;
    string strReply = JSONRPCReply(Value::null, objError, id);
    stream << HTTPReply(nStatus, strReply) << std::flush;
}

bool ClientAllowed(const string& strAddress)
{
    if (strAddress == asio::ip::address_v4::loopback().to_string())
        return true;
    const vector<string>& vAllow = mapMultiArgs["-rpcallowip"];
    BOOST_FOREACH(string strAllow, vAllow)
        if (WildcardMatch(strAddress, strAllow))
            return true;
    return false;
}

//
// IOStream device that speaks SSL but can also speak non-SSL
//
class SSLIOStreamDevice : public iostreams::device<iostreams::bidirectional> {
public:
    SSLIOStreamDevice(SSLStream &streamIn, bool fUseSSLIn) : stream(streamIn)
    {
        fUseSSL = fUseSSLIn;
        fNeedHandshake = fUseSSLIn;
    }

    void handshake(ssl::stream_base::handshake_type role)
    {
        if (!fNeedHandshake) return;
        fNeedHandshake = false;
        stream.handshake(role);
    }
    std::streamsize read(char* s, std::streamsize n)
    {
        handshake(ssl::stream_base::server); // HTTPS servers read first
        if (fUseSSL) return stream.read_some(asio::buffer(s, n));
        return stream.next_layer().read_some(asio::buffer(s, n));
    }
    std::streamsize write(const char* s, std::streamsize n)
    {
        handshake(ssl::stream_base::client); // HTTPS clients write first
        if (fUseSSL) return asio::write(stream, asio::buffer(s, n));
        return asio::write(stream.next_layer(), asio::buffer(s, n));
    }
    bool connect(const std::string& server, const std::string& port)
    {
        ip::tcp::resolver resolver(GET_IO_SERVICE(stream));
        ip::tcp::resolver::query query(server.c_str(), port.c_str());
        ip::tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);
        ip::tcp::resolver::iterator end;
        boost::system::error_code error = asio::error::host_not_found;
        while (error && endpoint_iterator != end)
        {
            stream.lowest_layer().close();
            stream.lowest_layer().connect(*endpoint_iterator++, error);
        }
        if (error)
            return false;
        return true;
    }

private:
    bool fNeedHandshake;
    bool fUseSSL;
    SSLStream& stream;
};

void ThreadRPCServer(void* parg)
{
    IMPLEMENT_RANDOMIZE_STACK(ThreadRPCServer(parg));

    // getwork/getblocktemplate mining rewards paid here:
    pMiningKey = new CReserveKey(pwalletMain);

    try
    {
        vnThreadsRunning[THREAD_RPCSERVER]++;
        ThreadRPCServer2(parg);
        vnThreadsRunning[THREAD_RPCSERVER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_RPCSERVER]--;
        PrintException(&e, "ThreadRPCServer()");
    } catch (...) {
        vnThreadsRunning[THREAD_RPCSERVER]--;
        PrintException(NULL, "ThreadRPCServer()");
    }

    delete pMiningKey; pMiningKey = NULL;

    printf("ThreadRPCServer exiting\n");
}

static Object JSONRPCExecOne(const Value& request)
{
    Object rpc_result;
    Object req = request.get_obj();
    Value id = Value::null;

    try {
        id = find_value(req, "id");

        // Parse method
        Value valMethod = find_value(req, "method");
        if (valMethod.type() == null_type)
            throw JSONRPCError(-32600, "Missing method");
        if (valMethod.type() != str_type)
            throw JSONRPCError(-32600, "Method must be a string");
        string strMethod = valMethod.get_str();
        if (strMethod != "getwork" && strMethod != "getblocktemplate")
            printf("ThreadRPCServer method=%s\n", strMethod.c_str());

        // Parse params
        Value valParams = find_value(req, "params");
        Array params;
        if (valParams.type() == array_type)
            params = valParams.get_array();
        else if (valParams.type() == null_type)
            params = Array();
        else
            throw JSONRPCError(-32600, "Params must be an array");

        Value result = tableRPC.execute(strMethod, params);
        rpc_result = JSONRPCReplyObj(result, Value::null, id);
    }
    catch (Object& objError)
    {
        rpc_result = JSONRPCReplyObj(Value::null, objError, id);
    }
    catch (std::exception& e)
    {
        rpc_result = JSONRPCReplyObj(Value::null, JSONRPCError(-32700, e.what()), id);
    }

    return rpc_result;
}

static string JSONRPCExecBatch(const Array& vReq)
{
    Array ret;
    for (unsigned int reqIdx = 0; reqIdx < vReq.size(); reqIdx++)
        ret.push_back(JSONRPCExecOne(vReq[reqIdx]));

    return write_string(Value(ret), false) + "\n";
}

void ThreadRPCServer2(void* parg)
{
    printf("ThreadRPCServer started\n");

    strRPCUserColonPass = mapArgs["-rpcuser"] + ":" + mapArgs["-rpcpassword"];
    if (mapArgs["-rpcpassword"] == "")
    {
        unsigned char rand_pwd[32];
        RAND_bytes(rand_pwd, 32);
        string strWhatAmI = "To use slimcoind";
        if (mapArgs.count("-server"))
            strWhatAmI = strprintf(_("To use the %s option"), "\"-server\"");
        else if (mapArgs.count("-daemon"))
            strWhatAmI = strprintf(_("To use the %s option"), "\"-daemon\"");
        ThreadSafeMessageBox(strprintf(
            _("%s, you must set a rpcpassword in the configuration file:\n %s\n"
              "It is recommended you use the following random password:\n"
              "rpcuser=bitcoinrpc\n"
              "rpcpassword=%s\n"
              "(you do not need to remember this password)\n"
              "If the file does not exist, create it with owner-readable-only file permissions.\n"),
                strWhatAmI.c_str(),
                GetConfigFile().string().c_str(),
                EncodeBase58(&rand_pwd[0],&rand_pwd[0]+32).c_str()),
            _("Error"), wxOK | wxMODAL);
        StartShutdown();
        return;
    }

    bool fUseSSL = GetBoolArg("-rpcssl");
    asio::ip::address bindAddress = mapArgs.count("-rpcallowip") ? asio::ip::address_v4::any() : asio::ip::address_v4::loopback();

    asio::io_service io_service;
    ip::tcp::endpoint endpoint(bindAddress, GetArg("-rpcport", fTestNet? TESTNET_RPC_PORT : RPC_PORT));
    ip::tcp::acceptor acceptor(io_service);
    try
    {
        acceptor.open(endpoint.protocol());
        acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen(socket_base::max_connections);
    }
    catch(boost::system::system_error &e)
    {
        ThreadSafeMessageBox(strprintf(_("An error occured while setting up the RPC port %i for listening: %s"), endpoint.port(), e.what()),
                             _("Error"), wxOK | wxMODAL);
        StartShutdown();
        return;
    }

#if BOOST_VERSION < 104800
    ssl::context context(io_service, ssl::context::sslv23);
#else
    /* GJH: ('cause it's crypto)
        Deprecated in 1.48
    http://www.boost.org/doc/libs/1_48_0/doc/html/boost_asio/reference/ssl__context.html
    context: Deprecated constructor taking a reference to an io_service object.
    */
    ssl::context context(ssl::context::sslv23);
#endif

    if (fUseSSL)
    {
        context.set_options(ssl::context::no_sslv2);

        filesystem::path pathCertFile(GetArg("-rpcsslcertificatechainfile", "server.cert"));
        if (!pathCertFile.is_complete()) pathCertFile = filesystem::path(GetDataDir()) / pathCertFile;
        if (filesystem::exists(pathCertFile)) context.use_certificate_chain_file(pathCertFile.string());
        else printf("ThreadRPCServer ERROR: missing server certificate file %s\n", pathCertFile.string().c_str());

        filesystem::path pathPKFile(GetArg("-rpcsslprivatekeyfile", "server.pem"));
        if (!pathPKFile.is_complete()) pathPKFile = filesystem::path(GetDataDir()) / pathPKFile;
        if (filesystem::exists(pathPKFile)) context.use_private_key_file(pathPKFile.string(), ssl::context::pem);
        else printf("ThreadRPCServer ERROR: missing server private key file %s\n", pathPKFile.string().c_str());

        string strCiphers = GetArg("-rpcsslciphers", "TLSv1+HIGH:!SSLv2:!aNULL:!eNULL:!AH:!3DES:@STRENGTH");

#if BOOST_VERSION < 104800
        SSL_CTX_set_cipher_list(context.impl(), strCiphers.c_str());
#else
        /* GJH: ('cause it's crypto)
        Deprecated in 1.48
        http://www.boost.org/doc/libs/1_48_0/doc/html/boost_asio/reference/ssl__context.html
        context.impl: (Deprecated: Use native_handle().) Get the underlying implementation in the native type.
        */
        SSL_CTX_set_cipher_list(context.native_handle(), strCiphers.c_str());
#endif


    }

    while (true)
    {
        // Accept connection
        SSLStream sslStream(io_service, context);
        SSLIOStreamDevice d(sslStream, fUseSSL);
        iostreams::stream<SSLIOStreamDevice> stream(d);

        ip::tcp::endpoint peer;
        vnThreadsRunning[THREAD_RPCSERVER]--;
        acceptor.accept(sslStream.lowest_layer(), peer);
        vnThreadsRunning[4]++;
        if (fShutdown)
            return;

        // Restrict callers by IP
        if (!ClientAllowed(peer.address().to_string()))
        {
            // Only send a 403 if we're not using SSL to prevent a DoS during the SSL handshake.
            if (!fUseSSL)
                stream << HTTPReply(403, "") << std::flush;
            continue;
        }

        map<string, string> mapHeaders;
        string strRequest;

        boost::thread api_caller(ReadHTTP, boost::ref(stream), boost::ref(mapHeaders), boost::ref(strRequest));
        if (!api_caller.timed_join(boost::posix_time::seconds(GetArg("-rpctimeout", 30))))
        {   // Timed out:
            acceptor.cancel();
            printf("ThreadRPCServer ReadHTTP timeout\n");
            continue;
        }

        // Check authorization
        if (mapHeaders.count("authorization") == 0)
        {
            stream << HTTPReply(401, "") << std::flush;
            continue;
        }
        if (!HTTPAuthorized(mapHeaders))
        {
            printf("ThreadRPCServer incorrect password attempt from %s\n",peer.address().to_string().c_str());
            /* Deter brute-forcing short passwords.
               If this results in a DOS the user really
               shouldn't have their RPC port exposed.*/
            if (mapArgs["-rpcpassword"].size() < 20)
                Sleep(250);

            stream << HTTPReply(401, "") << std::flush;
            continue;
        }

        Value id = Value::null;
        try
        {
            // Parse request
            Value valRequest;
            if (!read_string(strRequest, valRequest))
                throw JSONRPCError(RPC_PARSE_ERROR, "Parse error");
            string strReply;
            if (valRequest.type() == obj_type) {
              // singleton request
              Object result;
              result = JSONRPCExecOne(valRequest);
              strReply = write_string(Value(result), false) + "\n";
            } else if (valRequest.type() == array_type) {
              // array of requests
              strReply = JSONRPCExecBatch(valRequest.get_array());
            } else
              throw JSONRPCError(-32600, "Top-level object parse error");

            // Send reply
            stream << HTTPReply(200, strReply) << std::flush;
        }
        catch (Object& objError)
        {
            ErrorReply(stream, objError, id);
        }
        catch (std::exception& e)
        {
            ErrorReply(stream, JSONRPCError(RPC_PARSE_ERROR, e.what()), id);
        }
    }
}

json_spirit::Value CRPCTable::execute(const std::string &strMethod, const json_spirit::Array &params) const
{
    // Find method
    const CRPCCommand *pcmd = tableRPC[strMethod];
    if (!pcmd)
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found");

    // Observe safe mode
    string strWarning = GetWarnings("rpc");
    if (strWarning != "" && !GetBoolArg("-disablesafemode") &&
            !pcmd->okSafeMode)
        throw JSONRPCError(RPC_FORBIDDEN_BY_SAFE_MODE, string("Safe mode: ") + strWarning);


    try
    {
        // Execute
        Value result;
        {
            LOCK2(cs_main, pwalletMain->cs_wallet);
            result = pcmd->actor(params, false);
        }
        return result;
    }
    catch (std::exception& e)
    {
        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }
}

Object CallRPC(const string& strMethod, const Array& params)
{
    if (mapArgs["-rpcuser"] == "" && mapArgs["-rpcpassword"] == "")
        throw runtime_error(strprintf(
            _("You must set rpcpassword=<password> in the configuration file:\n%s\n"
              "If the file does not exist, create it with owner-readable-only file permissions."),
                GetConfigFile().string().c_str()));

    // Connect to localhost
    bool fUseSSL = GetBoolArg("-rpcssl");
    asio::io_service io_service;
#if BOOST_VERSION < 104800
      ssl::context context(io_service, ssl::context::sslv23);
#else
    /* GJH: ('cause it's crypto)
        Deprecated in 1.48
    http://www.boost.org/doc/libs/1_48_0/doc/html/boost_asio/reference/ssl__context.html
    context: Deprecated constructor taking a reference to an io_service object.
    */
    ssl::context context(ssl::context::sslv23);
#endif
    context.set_options(ssl::context::no_sslv2);
    SSLStream sslStream(io_service, context);
    SSLIOStreamDevice d(sslStream, fUseSSL);
    iostreams::stream<SSLIOStreamDevice> stream(d);
    if (!d.connect(GetArg("-rpcconnect", "127.0.0.1"), GetArg("-rpcport", CBigNum(fTestNet? TESTNET_RPC_PORT : RPC_PORT).ToString().c_str())))
        throw runtime_error("couldn't connect to server");

    // HTTP basic authentication
    string strUserPass64 = EncodeBase64(mapArgs["-rpcuser"] + ":" + mapArgs["-rpcpassword"]);
    map<string, string> mapRequestHeaders;
    mapRequestHeaders["Authorization"] = string("Basic ") + strUserPass64;

    // Send request
    string strRequest = JSONRPCRequest(strMethod, params, 1);
    string strPost = HTTPPost(strRequest, mapRequestHeaders);
    stream << strPost << std::flush;

    // Receive reply
    map<string, string> mapHeaders;
    string strReply;
    int nStatus = ReadHTTP(stream, mapHeaders, strReply);
    if (nStatus == 401)
        throw runtime_error("incorrect rpcuser or rpcpassword (authorization failed)");
    else if (nStatus >= 400 && nStatus != 400 && nStatus != 404 && nStatus != 500)
        throw runtime_error(strprintf("server returned HTTP error %d", nStatus));
    else if (strReply.empty())
        throw runtime_error("no response from server");

    // Parse reply
    Value valReply;
    if (!read_string(strReply, valReply))
        throw runtime_error("couldn't parse reply from server");
    const Object& reply = valReply.get_obj();
    if (reply.empty())
        throw runtime_error("expected reply to have result, error and id properties");

    return reply;
}


template<typename T>
void ConvertTo(Value& value, bool fAllowNull=false)
{
    if (fAllowNull && value.type() == null_type)
        return;
    if (value.type() == str_type)
    {
        // reinterpret string as unquoted json value
        Value value2;
        string strJSON = value.get_str();

        if (!read_string(strJSON, value2))
            throw runtime_error(string("Error parsing JSON:") + strJSON);
        ConvertTo<T>(value2, fAllowNull);
        value = value2;
    }
    else
    {
        value = value.get_value<T>();
    }
}

// Convert strings to command-specific RPC representation
Array RPCConvertValues(const std::string &strMethod, const std::vector<std::string> &strParams)
{
    // Parameters default to strings
    Array params;
    BOOST_FOREACH(const std::string &param, strParams)
        params.push_back(param);

    int n = params.size();

    //
    // Special case non-string parameter types
    //
    // if (strMethod == "setgenerate"            && n > 0) ConvertTo<bool>(params[0]);
    // if (strMethod == "setgenerate"            && n > 1) ConvertTo<boost::int64_t>(params[1]);
    FORMAT_PARAM("setgenerate", 0, bool);
    FORMAT_PARAM("setgenerate", 1, boost::int64_t);
    FORMAT_PARAM("makekeypair", 1, boost::int64_t);
    FORMAT_PARAM("importprivkey", 2, bool);
    if (strMethod == "sendtoaddress"          && n > 1) ConvertTo<double>(params[1]);
    if (strMethod == "settxfee"               && n > 0) ConvertTo<double>(params[0]);
    if (strMethod == "getreceivedbyaddress"   && n > 1) ConvertTo<boost::int64_t>(params[1]);
    if (strMethod == "getreceivedbyaccount"   && n > 1) ConvertTo<boost::int64_t>(params[1]);
    if (strMethod == "listreceivedbyaddress"  && n > 0) ConvertTo<boost::int64_t>(params[0]);
    if (strMethod == "listreceivedbyaddress"  && n > 1) ConvertTo<bool>(params[1]);
    if (strMethod == "listreceivedbyaccount"  && n > 0) ConvertTo<boost::int64_t>(params[0]);
    if (strMethod == "listreceivedbyaccount"  && n > 1) ConvertTo<bool>(params[1]);
    if (strMethod == "getbalance"             && n > 1) ConvertTo<boost::int64_t>(params[1]);
    if (strMethod == "getblockhash"           && n > 0) ConvertTo<boost::int64_t>(params[0]);
    if (strMethod == "getblock"               && n > 1) ConvertTo<bool>(params[1]);
    if (strMethod == "getblock"               && n > 2) ConvertTo<bool>(params[2]);
    if (strMethod == "move"                   && n > 2) ConvertTo<double>(params[2]);
    if (strMethod == "move"                   && n > 3) ConvertTo<boost::int64_t>(params[3]);
    if (strMethod == "sendfrom"               && n > 2) ConvertTo<double>(params[2]);
    if (strMethod == "sendfrom"               && n > 3) ConvertTo<boost::int64_t>(params[3]);
    if (strMethod == "calcburnhash"           && n > 0) ConvertTo<bool>(params[0]);
    if (strMethod == "burncoins"              && n > 1) ConvertTo<double>(params[1]);
    if (strMethod == "burncoins"              && n > 2) ConvertTo<boost::int64_t>(params[2]);
    if (strMethod == "listtransactions"       && n > 1) ConvertTo<bool>(params[1]);
    if (strMethod == "listtransactions"       && n > 2) ConvertTo<boost::int64_t>(params[2]);
    if (strMethod == "listtransactions"       && n > 3) ConvertTo<boost::int64_t>(params[3]);
    if (strMethod == "listburnminted"         && n > 0) ConvertTo<bool>(params[0]);
    if (strMethod == "listaccounts"           && n > 0) ConvertTo<boost::int64_t>(params[0]);
    if (strMethod == "walletpassphrase"       && n > 1) ConvertTo<boost::int64_t>(params[1]);
    if (strMethod == "walletpassphrase"       && n > 2) ConvertTo<bool>(params[2]);
    if (strMethod == "getblocktemplate"       && n > 0) ConvertTo<Object>(params[0]);
    if (strMethod == "dumpbootstrap"          && n > 1) ConvertTo<boost::int64_t>(params[1]);
    if (strMethod == "dumpbootstrap"          && n > 2) ConvertTo<boost::int64_t>(params[2]);
    if (strMethod == "listsinceblock"         && n > 1) ConvertTo<boost::int64_t>(params[1]);
    if (strMethod == "sendalert"              && n > 2) ConvertTo<boost::int64_t>(params[2]);
    if (strMethod == "sendalert"              && n > 3) ConvertTo<boost::int64_t>(params[3]);
    if (strMethod == "sendalert"              && n > 4) ConvertTo<boost::int64_t>(params[4]);
    if (strMethod == "sendalert"              && n > 5) ConvertTo<boost::int64_t>(params[5]);
    if (strMethod == "sendalert"              && n > 6) ConvertTo<boost::int64_t>(params[6]);
    if (strMethod == "sendmany"               && n > 1)
    {
        string s = params[1].get_str();
        Value v;
        if (!read_string(s, v) || v.type() != obj_type)
            throw runtime_error("type mismatch");
        params[1] = v.get_obj();
    }
    if (strMethod == "sendmany"                && n > 2) ConvertTo<boost::int64_t>(params[2]);
    if (strMethod == "reservebalance"          && n > 0) ConvertTo<bool>(params[0]);
    if (strMethod == "reservebalance"          && n > 1) ConvertTo<double>(params[1]);
    if (strMethod == "addmultisigaddress"      && n > 0) ConvertTo<boost::int64_t>(params[0]);
    if (strMethod == "addmultisigaddress"      && n > 1)
    {
        string s = params[1].get_str();
        Value v;
        if (!read_string(s, v) || v.type() != array_type)
            throw runtime_error("type mismatch "+s);
        params[1] = v.get_array();
    }
    if (strMethod == "listunspent"            && n > 0) ConvertTo<boost::int64_t>(params[0]);
    if (strMethod == "listunspent"            && n > 1) ConvertTo<boost::int64_t>(params[1]);
    if (strMethod == "listunspent"            && n > 2) ConvertTo<Array>(params[2]);
    if (strMethod == "getrawtransaction"      && n > 1) ConvertTo<boost::int64_t>(params[1]);
    if (strMethod == "createrawtransaction"   && n > 0) ConvertTo<Array>(params[0]);
    if (strMethod == "createrawtransaction"   && n > 1) ConvertTo<Object>(params[1]);
    if (strMethod == "signrawtransaction"     && n > 1) ConvertTo<Array>(params[1]);
    if (strMethod == "signrawtransaction"     && n > 2) ConvertTo<Array>(params[2]);
    if (strMethod == "sendrawtransaction"     && n > 1) ConvertTo<boost::int64_t>(params[1]);

    FORMAT_PARAM("getrawtransaction",  1, boost::int64_t);
    if (strMethod == "signrawtransaction"     && n > 1) ConvertTo<Array>(params[1], true);
    if (strMethod == "signrawtransaction"     && n > 2) ConvertTo<Array>(params[2], true);
    return params;
}

int CommandLineRPC(int argc, char *argv[])
{
    string strPrint;
    int nRet = 0;
    try
    {
        // Skip switches
        while (argc > 1 && IsSwitchChar(argv[1][0]))
        {
            argc--;
            argv++;
        }

        // Method
        if (argc < 2)
            throw runtime_error("too few parameters");
        string strMethod = argv[1];

        // Parameters default to strings
        std::vector<std::string> strParams(&argv[2], &argv[argc]);
        Array params = RPCConvertValues(strMethod, strParams);

        // Execute
        Object reply = CallRPC(strMethod, params);

        // Parse reply
        const Value& result = find_value(reply, "result");
        const Value& error  = find_value(reply, "error");

        if (error.type() != null_type)
        {
            // Error
            strPrint = "error: " + write_string(error, false);
            int code = find_value(error.get_obj(), "code").get_int();
            nRet = abs(code);
        }
        else
        {
            // Result
            if (result.type() == null_type)
                strPrint = "";
            else if (result.type() == str_type)
                strPrint = result.get_str();
            else
                strPrint = write_string(result, true);
        }
    }
    catch (std::exception& e)
    {
        strPrint = string("error: ") + e.what();
        nRet = 87;
    }
    catch (...)
    {
        PrintException(NULL, "CommandLineRPC()");
    }

    if (strPrint != "")
    {
        fprintf((nRet == 0 ? stdout : stderr), "%s\n", strPrint.c_str());
    }
    return nRet;
}

#ifdef TEST
int main(int argc, char *argv[])
{
#ifdef _MSC_VER
    // Turn off microsoft heap dump noise
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, CreateFile("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0));
#endif
    setbuf(stdin, NULL);
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    try
    {
        if (argc >= 2 && string(argv[1]) == "-server")
        {
            printf("server ready\n");
            ThreadRPCServer(NULL);
        }
        else
        {
            return CommandLineRPC(argc, argv);
        }
    }
    catch (std::exception& e) {
        PrintException(&e, "main()");
    } catch (...) {
        PrintException(NULL, "main()");
    }
    return 0;
}
#endif

const CRPCTable tableRPC;

// Return average network hashes per second based on the last 'lookup' blocks,
// or from the last difficulty change if 'lookup' is nonpositive.
// If 'height' is nonnegative, compute the estimate at the time when a given block was found.

double GetNetworkHashPS(int lookup, int height) {
    CBlockIndex *pb = pindexBest;

    if (height >= 0 && height < nBestHeight)
        pb = FindBlockByHeight(height);

    if (pb == NULL || !pb->nHeight)
        return 0;

    // If lookup is -1, then use blocks since last difficulty change.
    if (lookup <= 0)
        lookup = pb->nHeight % 2016 + 1;

    // If lookup is larger than chain, then set it to chain length.
    if (lookup > pb->nHeight)
        lookup = pb->nHeight;

    CBlockIndex *pb0 = pb;
    int64 minTime = pb0->GetBlockTime();
    int64 maxTime = minTime;
    for (int i = 0; i < lookup; i++) {
        pb0 = pb0->pprev;
        int64 time = pb0->GetBlockTime();
        minTime = std::min(time, minTime);
        maxTime = std::max(time, maxTime);
    }

    // In case there's a situation where minTime == maxTime, we don't want a divide by zero exception.
    if (minTime == maxTime)
        return 0;

    // uint256 workDiff = pb->nChainWork - pb0->nChainWork;
    CBigNum workDiff = pb->bnChainTrust - pb0->bnChainTrust;
    int64 timeDiff = maxTime - minTime;

    return (boost::int64_t)(workDiff.getuint64() / timeDiff);
}

Value getnetworkhashps(const Array& params, bool fHelp)
{
    if (fHelp || params.size() > 2)
        throw runtime_error(
            "getnetworkhashps [blocks] [height]\n"
            "Returns the estimated network hashes per second based on the last 120 blocks.\n"
            "Pass in [blocks] to override # of blocks, -1 specifies since last difficulty change.\n"
            "Pass in [height] to estimate the network speed at the time when a certain block was found.");

    return GetNetworkHashPS(params.size() > 0 ? params[0].get_int() : 120, params.size() > 1 ? params[1].get_int() : -1);
}
