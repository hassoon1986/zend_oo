// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "consensus/validation.h"
#include "core_io.h"
#include "init.h"
#include "keystore.h"
#include "main.h"
#include "merkleblock.h"
#include "net.h"
#include "primitives/transaction.h"
#include "rpc/server.h"
#include "script/script.h"
#include "script/script_error.h"
#include "script/sign.h"
#include "script/standard.h"
#include "uint256.h"
#include "tinyformat.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include <stdint.h>
#include <string>

#include <boost/assign/list_of.hpp>

#include <univalue.h>
#include "sc/sidechain.h"
#include "sc/sidechainrpc.h"

using namespace std;

void ScriptPubKeyToJSON(const CScript& scriptPubKey, UniValue& out, bool fIncludeHex)
{
    txnouttype type;
    vector<CTxDestination> addresses;
    int nRequired;

    out.push_back(Pair("asm", scriptPubKey.ToString()));
    if (fIncludeHex)
        out.push_back(Pair("hex", HexStr(scriptPubKey.begin(), scriptPubKey.end())));

    if (!ExtractDestinations(scriptPubKey, type, addresses, nRequired)) {
        out.push_back(Pair("type", GetTxnOutputType(type)));
        return;
    }

    out.push_back(Pair("reqSigs", nRequired));
    out.push_back(Pair("type", GetTxnOutputType(type)));

    UniValue a(UniValue::VARR);
    BOOST_FOREACH(const CTxDestination& addr, addresses)
        a.push_back(CBitcoinAddress(addr).ToString());
    out.push_back(Pair("addresses", a));
}


UniValue TxJoinSplitToJSON(const CTransaction& tx) {
    bool useGroth = tx.nVersion == GROTH_TX_VERSION;
    UniValue vjoinsplit(UniValue::VARR);
    for (unsigned int i = 0; i < tx.GetVjoinsplit().size(); i++) {
        const JSDescription& jsdescription = tx.GetVjoinsplit()[i];
        UniValue joinsplit(UniValue::VOBJ);

        joinsplit.push_back(Pair("vpub_old", ValueFromAmount(jsdescription.vpub_old)));
        joinsplit.push_back(Pair("vpub_new", ValueFromAmount(jsdescription.vpub_new)));

        joinsplit.push_back(Pair("anchor", jsdescription.anchor.GetHex()));

        {
            UniValue nullifiers(UniValue::VARR);
            BOOST_FOREACH(const uint256 nf, jsdescription.nullifiers) {
                nullifiers.push_back(nf.GetHex());
            }
            joinsplit.push_back(Pair("nullifiers", nullifiers));
        }

        {
            UniValue commitments(UniValue::VARR);
            BOOST_FOREACH(const uint256 commitment, jsdescription.commitments) {
                commitments.push_back(commitment.GetHex());
            }
            joinsplit.push_back(Pair("commitments", commitments));
        }

        joinsplit.push_back(Pair("onetimePubKey", jsdescription.ephemeralKey.GetHex()));
        joinsplit.push_back(Pair("randomSeed", jsdescription.randomSeed.GetHex()));

        {
            UniValue macs(UniValue::VARR);
            BOOST_FOREACH(const uint256 mac, jsdescription.macs) {
                macs.push_back(mac.GetHex());
            }
            joinsplit.push_back(Pair("macs", macs));
        }

        CDataStream ssProof(SER_NETWORK, PROTOCOL_VERSION);
        auto ps = SproutProofSerializer<CDataStream>(ssProof, useGroth, SER_NETWORK, PROTOCOL_VERSION);
        boost::apply_visitor(ps, jsdescription.proof);
        joinsplit.push_back(Pair("proof", HexStr(ssProof.begin(), ssProof.end())));

        {
            UniValue ciphertexts(UniValue::VARR);
            for (const ZCNoteEncryption::Ciphertext ct : jsdescription.ciphertexts) {
                ciphertexts.push_back(HexStr(ct.begin(), ct.end()));
            }
            joinsplit.push_back(Pair("ciphertexts", ciphertexts));
        }

        vjoinsplit.push_back(joinsplit);
    }
    return vjoinsplit;
}

void TxToJSON(const CTransaction& tx, const uint256 hashBlock, UniValue& entry)
{
    entry.push_back(Pair("txid", tx.GetHash().GetHex()));
    entry.push_back(Pair("version", tx.nVersion));
    entry.push_back(Pair("locktime", (int64_t)tx.GetLockTime()));
    UniValue vin(UniValue::VARR);
    BOOST_FOREACH(const CTxIn& txin, tx.GetVin()) {
        UniValue in(UniValue::VOBJ);
        if (tx.IsCoinBase())
            in.push_back(Pair("coinbase", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
        else {
            in.push_back(Pair("txid", txin.prevout.hash.GetHex()));
            in.push_back(Pair("vout", (int64_t)txin.prevout.n));
            UniValue o(UniValue::VOBJ);
            o.push_back(Pair("asm", txin.scriptSig.ToString()));
            o.push_back(Pair("hex", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
            in.push_back(Pair("scriptSig", o));
        }
        in.push_back(Pair("sequence", (int64_t)txin.nSequence));
        vin.push_back(in);
    }
    entry.push_back(Pair("vin", vin));
    UniValue vout(UniValue::VARR);
    for (unsigned int i = 0; i < tx.GetVout().size(); i++) {
        const CTxOut& txout = tx.GetVout()[i];
        UniValue out(UniValue::VOBJ);
        out.push_back(Pair("value", ValueFromAmount(txout.nValue)));
        out.push_back(Pair("valueZat", txout.nValue));
        out.push_back(Pair("n", (int64_t)i));
        UniValue o(UniValue::VOBJ);
        ScriptPubKeyToJSON(txout.scriptPubKey, o, true);
        out.push_back(Pair("scriptPubKey", o));
        vout.push_back(out);
    }
    entry.push_back(Pair("vout", vout));

    // add to entry obj the cross chain outputs
    Sidechain::AddSidechainOutsToJSON(tx, entry);

    UniValue vjoinsplit = TxJoinSplitToJSON(tx);
    entry.push_back(Pair("vjoinsplit", vjoinsplit));

    if (!hashBlock.IsNull()) {
        entry.push_back(Pair("blockhash", hashBlock.GetHex()));
        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex* pindex = (*mi).second;
            if (chainActive.Contains(pindex)) {
                entry.push_back(Pair("confirmations", 1 + chainActive.Height() - pindex->nHeight));
                entry.push_back(Pair("time", pindex->GetBlockTime()));
                entry.push_back(Pair("blocktime", pindex->GetBlockTime()));
            }
            else
                entry.push_back(Pair("confirmations", 0));
        }
    }
}

void CertToJSON(const CScCertificate& cert, const uint256 hashBlock, UniValue& entry)
{
    entry.push_back(Pair("certid", cert.GetHash().GetHex()));
    entry.push_back(Pair("version", cert.nVersion));
    UniValue vin(UniValue::VARR);
    BOOST_FOREACH(const CTxIn& txin, cert.GetVin()) {
        UniValue in(UniValue::VOBJ);
        in.push_back(Pair("txid", txin.prevout.hash.GetHex()));
        in.push_back(Pair("vout", (int64_t)txin.prevout.n));
        UniValue o(UniValue::VOBJ);
        o.push_back(Pair("asm", txin.scriptSig.ToString()));
        o.push_back(Pair("hex", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
        in.push_back(Pair("scriptSig", o));
        in.push_back(Pair("sequence", (int64_t)txin.nSequence));
        vin.push_back(in);
    }
    entry.push_back(Pair("vin", vin));
    UniValue vout(UniValue::VARR);
    for (unsigned int i = 0; i < cert.GetVout().size(); i++) {
        const CTxOut& txout = cert.GetVout()[i];
        UniValue out(UniValue::VOBJ);
        out.push_back(Pair("value", ValueFromAmount(txout.nValue)));
        out.push_back(Pair("valueZat", txout.nValue));
        out.push_back(Pair("n", (int64_t)i));
        UniValue o(UniValue::VOBJ);
        ScriptPubKeyToJSON(txout.scriptPubKey, o, true);
        out.push_back(Pair("scriptPubKey", o));
        if (cert.IsBackwardTransfer(i))
        {
            std::string pkhStr;
            auto it = std::find(txout.scriptPubKey.begin(), txout.scriptPubKey.end(), OP_HASH160);
            if (it != txout.scriptPubKey.end())
            {
                it += 2;
                std::vector<unsigned char> pkh(it, it + sizeof(uint160));
                pkhStr = HexStr(pkh.rbegin(), pkh.rend());
            }
            else
            {
                pkhStr = "<<Decode error>>";
            }
            out.push_back(Pair("backward transfer", true));
            out.push_back(Pair("pubkeyhash", pkhStr));
        }
        vout.push_back(out);
    }

    UniValue x(UniValue::VOBJ);
    x.push_back(Pair("scid", cert.GetScId().GetHex()));
    x.push_back(Pair("epochNumber", cert.epochNumber));
    x.push_back(Pair("quality", cert.quality));
    x.push_back(Pair("endEpochBlockHash", cert.endEpochBlockHash.GetHex()));
    x.push_back(Pair("scProof", HexStr(cert.scProof)));
    x.push_back(Pair("totalAmount", ValueFromAmount(cert.GetValueOfBackwardTransfers())));

    entry.push_back(Pair("cert", x));
    entry.push_back(Pair("vout", vout));

    if (!hashBlock.IsNull()) {
        entry.push_back(Pair("blockhash", hashBlock.GetHex()));
        BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex* pindex = (*mi).second;
            if (chainActive.Contains(pindex)) {
                entry.push_back(Pair("confirmations", 1 + chainActive.Height() - pindex->nHeight));
                entry.push_back(Pair("blocktime", pindex->GetBlockTime()));
            }
            else
            {
                entry.push_back(Pair("confirmations", 0));
            }
        }
    }
}

UniValue getrawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getrawtransaction \"txid\" ( verbose )\n"
            "\nNOTE: By default this function only works sometimes. This is when the tx is in the mempool\n"
            "or there is an unspent output in the utxo for this transaction. To make it always work,\n"
            "you need to maintain a transaction index, using the -txindex command line option.\n"
            "\nReturn the raw transaction data.\n"
            "\nIf verbose=0, returns a string that is serialized, hex-encoded data for 'txid'.\n"
            "If verbose is non-zero, returns an Object with information about 'txid'.\n"

            "\nArguments:\n"
            "1. \"txid\"      (string, required) The transaction id\n"
            "2. verbose       (numeric, optional, default=0) If 0, return a string, other return a json object\n"

            "\nResult (if verbose is not set or set to 0):\n"
            "\"data\"      (string) The serialized, hex-encoded data for 'txid'\n"

            "\nResult (if verbose > 0):\n"
            "{\n"
            "  \"hex\" : \"data\",       (string) The serialized, hex-encoded data for 'txid'\n"
            "  \"txid\" : \"id\",        (string) The transaction id (same as provided)\n"
            "  \"version\" : n,          (numeric) The version\n"
            "  \"locktime\" : ttt,       (numeric) The lock time\n"
            "  \"vin\" : [               (array of json objects)\n"
            "     {\n"
            "       \"txid\": \"id\",    (string) The transaction id\n"
            "       \"vout\": n,         (numeric) \n"
            "       \"scriptSig\": {     (json object) The script\n"
            "         \"asm\": \"asm\",  (string) asm\n"
            "         \"hex\": \"hex\"   (string) hex\n"
            "       },\n"
            "       \"sequence\": n      (numeric) The script sequence number\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vout\" : [              (array of json objects)\n"
            "     {\n"
            "       \"value\" : x.xxx,            (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"n\" : n,                    (numeric) index\n"
            "       \"scriptPubKey\" : {          (json object)\n"
            "         \"asm\" : \"asm\",          (string) the asm\n"
            "         \"hex\" : \"hex\",          (string) the hex\n"
            "         \"reqSigs\" : n,            (numeric) The required sigs\n"
            "         \"type\" : \"pubkeyhash\",  (string) The type, eg 'pubkeyhash'\n"
            "         \"addresses\" : [           (json array of string)\n"
            "           \"horizenaddress\"          (string) Horizen address\n"
            "           ,...\n"
            "         ]\n"
            "       }\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vjoinsplit\" : [        (array of json objects, only for version >= 2)\n"
            "     {\n"
            "       \"vpub_old\" : x.xxx,         (numeric) public input value in " + CURRENCY_UNIT + "\n"
            "       \"vpub_new\" : x.xxx,         (numeric) public output value in " + CURRENCY_UNIT + "\n"
            "       \"anchor\" : \"hex\",         (string) the anchor\n"
            "       \"nullifiers\" : [            (json array of string)\n"
            "         \"hex\"                     (string) input note nullifier\n"
            "         ,...\n"
            "       ],\n"
            "       \"commitments\" : [           (json array of string)\n"
            "         \"hex\"                     (string) output note commitment\n"
            "         ,...\n"
            "       ],\n"
            "       \"onetimePubKey\" : \"hex\",  (string) the onetime public key used to encrypt the ciphertexts\n"
            "       \"randomSeed\" : \"hex\",     (string) the random seed\n"
            "       \"macs\" : [                  (json array of string)\n"
            "         \"hex\"                     (string) input note MAC\n"
            "         ,...\n"
            "       ],\n"
            "       \"proof\" : \"hex\",          (string) the zero-knowledge proof\n"
            "       \"ciphertexts\" : [           (json array of string)\n"
            "         \"hex\"                     (string) output note ciphertext\n"
            "         ,...\n"
            "       ]\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"blockhash\" : \"hash\",   (string) the block hash\n"
            "  \"confirmations\" : n,      (numeric) The confirmations\n"
            "  \"time\" : ttt,             (numeric) The transaction time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"blocktime\" : ttt         (numeric) The block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("getrawtransaction", "\"mytxid\"")
            + HelpExampleCli("getrawtransaction", "\"mytxid\" 1")
            + HelpExampleRpc("getrawtransaction", "\"mytxid\", 1")
        );
    LOCK(cs_main);

    uint256 hash = ParseHashV(params[0], "parameter 1");

    bool fVerbose = false;
    if (params.size() > 1)
        fVerbose = (params[1].get_int() != 0);

    CTransaction tx;
    uint256 hashBlock;
    if (!GetTransaction(hash, tx, hashBlock, true))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available about transaction");

    string strHex = EncodeHexTx(tx);

    if (!fVerbose)
        return strHex;

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hex", strHex));
    TxToJSON(tx, hashBlock, result);
    return result;
}

UniValue getrawcertificate(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "getrawcertificate \"certid\" ( verbose )\n"
            "\nNOTE: By default this function only works sometimes. This is when the certificate is in the mempool\n"
            "or there is an unspent output in the utxo for this certificate. To make it always work,\n"
            "you need to maintain a transaction index, using the -txindex command line option.\n"
            "\nReturn the raw certificate data.\n"
            "\nIf verbose=0, returns a string that is serialized, hex-encoded data for 'certid'.\n"
            "If verbose is non-zero, returns an Object with information about 'certid'.\n"

            "\nArguments:\n"
            "1. \"certid\"      (string, required) The certificate id\n"
            "2. verbose       (numeric, optional, default=0) If 0, return a string, other return a json object\n"

            "\nResult (if verbose is not set or set to 0):\n"
            "\"data\"      (string) The serialized, hex-encoded data for 'certid'\n"

            "\nResult (if verbose > 0):\n"
            "{\n"
            "  \"hex\" : \"data\",         (string) The serialized, hex-encoded data for 'certid'\n"
            "  \"certid\" : \"id\",        (string) The transaction id (same as provided)\n"
            "  \"version\" : n,          (numeric) The version\n"
            "  \"cert\" :                (json object)\n"
            "     {\n"
            "       \"scid\" : \"sc id\",              (string) the sidechain id\n"
            "       \"epochNumber\": epn,            (numeric) the withdrawal epoch number this certificate refers to\n"
            "       \"quality\": n,                  (numeric) the quality of this withdrawal certificate. \n"
            "       \"endEpochBlockHash\" : \"eph\"    (string) the hash of the block marking the end of the abovementioned epoch\n"
            "       \"scProof\": \"scp\"               (string) SNARK proof whose verification key wCertVk was set upon sidechain registration\n"
            "       \"totalAmount\" : x.xxx         (numeric) The total value of the certificate in " + CURRENCY_UNIT + "\n"
            "     }\n"
            "  \"vout\" : [              (array of json objects)\n"
            "     {\n"
            "       \"value\" : x.xxx,            (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"valueZat\" : xxxx,          (numeric) The value in Zat\n"
            "       \"n\" : n,                    (numeric) index\n"
            "       \"scriptPubKey\" : {          (json object)\n"
            "         \"asm\" : \"asm\",            (string) the asm\n"
            "         \"hex\" : \"hex\",            (string) the hex\n"
            "         \"type\" : \"pubkeyhash\",    (string) The type, eg 'pubkeyhash'\n"
            "         \"addresses\" : [           (json array of string)\n"
            "           \"horizenaddress\"        (string) Horizen address\n"
            "           ,...\n"
            "         ]\n"
            "       }\n"
            "       --- optional fields present only if this vout is a backward transfer:\n" 
            "       \"backward transfer\" : true  (bool)\n" 
            "       \"pubkeyhash\" : \"pkh\"        (string) public key hash this backward transfer refers to, it corresponds to the horizen address specified above"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"blockhash\" : \"hash\",   (string) the block hash\n"
            "  \"confirmations\" : n,    (numeric) The confirmations\n"
            "  \"blocktime\" : ttt       (numeric) The block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("getrawcertificate", "\"mycertid\"")
            + HelpExampleCli("getrawcertificate", "\"mycertid\" 1")
            + HelpExampleRpc("getrawcertificate", "\"mycertid\", 1")
        );
    LOCK(cs_main);

    uint256 hash = ParseHashV(params[0], "parameter 1");

    bool fVerbose = false;
    if (params.size() > 1)
        fVerbose = (params[1].get_int() != 0);

    CScCertificate cert;
    uint256 hashBlock;
    if (!GetCertificate(hash, cert, hashBlock, true))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "No information available about certificate");

    string strHex = EncodeHexCert(cert);

    if (!fVerbose)
        return strHex;

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hex", strHex));
    CertToJSON(cert, hashBlock, result);
    return result;
}

UniValue gettxoutproof(const UniValue& params, bool fHelp)
{
    if (fHelp || (params.size() != 1 && params.size() != 2))
        throw runtime_error(
            "gettxoutproof [\"txid\",...] ( blockhash )\n"
            "\nReturns a hex-encoded proof that \"txid\" was included in a block.\n"
            "\nNOTE: By default this function only works sometimes. This is when there is an\n"
            "unspent output in the utxo for this transaction. To make it always work,\n"
            "you need to maintain a transaction index, using the -txindex command line option or\n"
            "specify the block in which the transaction is included in manually (by blockhash).\n"
            "\nReturn the raw transaction data.\n"
            "\nArguments:\n"
            "1. \"txids\"       (string) A json array of txids to filter\n"
            "    [\n"
            "      \"txid\"     (string) A transaction hash\n"
            "      ,...\n"
            "    ]\n"
            "2. \"block hash\"  (string, optional) If specified, looks for txid in the block with this hash\n"
            "\nResult:\n"
            "\"data\"           (string) A string that is a serialized, hex-encoded data for the proof.\n"
        );

    set<uint256> setTxids;
    uint256 oneTxid;
    UniValue txids = params[0].get_array();
    for (size_t idx = 0; idx < txids.size(); idx++) {
        const UniValue& txid = txids[idx];
        if (txid.get_str().length() != 64 || !IsHex(txid.get_str()))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid txid ")+txid.get_str());
        uint256 hash(uint256S(txid.get_str()));
        if (setTxids.count(hash))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated txid: ")+txid.get_str());
       setTxids.insert(hash);
       oneTxid = hash;
    }

    LOCK(cs_main);

    CBlockIndex* pblockindex = NULL;

    uint256 hashBlock;
    if (params.size() > 1)
    {
        hashBlock = uint256S(params[1].get_str());
        if (!mapBlockIndex.count(hashBlock))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        pblockindex = mapBlockIndex[hashBlock];
    } else {
        CCoins coins;
        if (pcoinsTip->GetCoins(oneTxid, coins) && coins.nHeight > 0 && coins.nHeight <= chainActive.Height())
            pblockindex = chainActive[coins.nHeight];
    }

    if (pblockindex == NULL)
    {
        CTransaction tx;
        if (!GetTransaction(oneTxid, tx, hashBlock, false) || hashBlock.IsNull())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not yet in block");
        if (!mapBlockIndex.count(hashBlock))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Transaction index corrupt");
        pblockindex = mapBlockIndex[hashBlock];
    }

    CBlock block;
    if(!ReadBlockFromDisk(block, pblockindex))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Can't read block from disk");

    unsigned int ntxFound = 0;
    BOOST_FOREACH(const CTransaction&tx, block.vtx)
        if (setTxids.count(tx.GetHash()))
            ntxFound++;
    if (ntxFound != setTxids.size())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "(Not all) transactions not found in specified block");

    CDataStream ssMB(SER_NETWORK, PROTOCOL_VERSION);
    CMerkleBlock mb(block, setTxids);
    ssMB << mb;
    std::string strHex = HexStr(ssMB.begin(), ssMB.end());
    return strHex;
}

UniValue verifytxoutproof(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "verifytxoutproof \"proof\"\n"
            "\nVerifies that a proof points to a transaction in a block, returning the transaction it commits to\n"
            "and throwing an RPC error if the block is not in our best chain\n"
            "\nArguments:\n"
            "1. \"proof\"    (string, required) The hex-encoded proof generated by gettxoutproof\n"
            "\nResult:\n"
            "[\"txid\"]      (array, strings) The txid(s) which the proof commits to, or empty array if the proof is invalid\n"
        );

    CDataStream ssMB(ParseHexV(params[0], "proof"), SER_NETWORK, PROTOCOL_VERSION);
    CMerkleBlock merkleBlock;
    ssMB >> merkleBlock;

    UniValue res(UniValue::VARR);

    vector<uint256> vMatch;
    if (merkleBlock.txn.ExtractMatches(vMatch) != merkleBlock.header.hashMerkleRoot)
        return res;

    LOCK(cs_main);

    if (!mapBlockIndex.count(merkleBlock.header.GetHash()) || !chainActive.Contains(mapBlockIndex[merkleBlock.header.GetHash()]))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found in chain");

    BOOST_FOREACH(const uint256& hash, vMatch)
        res.push_back(hash.GetHex());
    return res;
}

void AddInputsToRawObject(CMutableTransactionBase& rawTxObj, const UniValue& inputs)
{
    // inputs
    for (size_t idx = 0; idx < inputs.size(); idx++) {
        const UniValue& input = inputs[idx];
        const UniValue& o = input.get_obj();

        uint256 txid = ParseHashO(o, "txid");

        const UniValue& vout_v = find_value(o, "vout");
        if (!vout_v.isNum())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, missing vout key");
        int nOutput = vout_v.get_int();
        if (nOutput < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");

        CTxIn in(COutPoint(txid, nOutput));
        rawTxObj.vin.push_back(in);
    }
}

void AddOutputsToRawObject(CMutableTransactionBase& rawTxObj, const UniValue& sendTo)
{
    set<CBitcoinAddress> setAddress;
    vector<string> addrList = sendTo.getKeys();
    BOOST_FOREACH(const string& name_, addrList) {
        CBitcoinAddress address(name_);
        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Horizen address: ")+name_);

        if (setAddress.count(address))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+name_);
        setAddress.insert(address);

        CScript scriptPubKey = GetScriptForDestination(address.Get());
        CAmount nAmount = AmountFromValue(sendTo[name_]);

        rawTxObj.addOut(CTxOut(nAmount, scriptPubKey));
    }
}

UniValue createrawtransaction(const UniValue& params, bool fHelp)
{   
    if (fHelp || params.size() > 4)
        throw runtime_error(
            "createrawtransaction [{\"txid\":\"id\",\"vout\":n},...] {\"address\":amount,...} ( [{epoch_length\":h, \"address\":\"address\", \"amount\":amount, \"wCertVk\":hexstr, \"customData\":hexstr, \"constant\":hexstr},...] ( [{\"address\":\"address\", \"amount\":amount, \"scid\":id}] ) )\n"
            "\nCreate a transaction spending the given inputs and sending to the given addresses.\n"
            "Returns hex-encoded raw transaction.\n"
            "Note that the transaction's inputs are not signed, and\n"
            "it is not stored in the wallet or transmitted to the network.\n"
            "See also \"fundrawtransaction\" RPC method.\n"

            "\nArguments:\n"
            "1. \"transactions\"        (string, required) A json array of json objects\n"
            "     [\n"
            "       {\n"
            "         \"txid\":\"id\",  (string, required) The transaction id\n"
            "         \"vout\":n        (numeric, required) The output number\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"
            "2. \"addresses\"           (string, required) a json object with addresses as keys and amounts as values\n"
            "    {\n"
            "      \"address\": x.xxx   (numeric, required) The key is the Horizen address, the value is the " + CURRENCY_UNIT + " amount\n"
            "      ,...\n"
            "    }\n"
            "3. \"sc creations\"        (string, optional but required if 4 is also given) A json array of json objects\n"
            "     [\n"
            "       {\n"
            "         \"epoch_length\":n (numeric, required) length of the withdrawal epochs\n"
            "         \"address\":\"address\",  (string, required) The receiver PublicKey25519Proposition in the SC\n"
            "         \"amount\":amount         (numeric, required) The numeric amount in " + CURRENCY_UNIT + " is the value\n"
            "         \"wCertVk\":hexstr          (string, required) It is an arbitrary byte string of even length expressed in\n"
            "                                       hexadecimal format. Required to verify a WCert SC proof. Its size must be " + strprintf("%d", SC_VK_SIZE) + " bytes\n"
            "         \"customData\":hexstr       (string, optional) It is an arbitrary byte string of even length expressed in\n"
            "                                       hexadecimal format. A max limit of 1024 bytes will be checked\n"
            "         \"constant\":hexstr         (string, optional) It is an arbitrary byte string of even length expressed in\n"
            "                                       hexadecimal format. Used as public input for WCert proof verification. Its size must be " + strprintf("%d", SC_FIELD_SIZE) + " bytes\n"            "       }\n"
            "       ,...\n"
            "     ]\n"
            "4. \"forward transfers\"   (string, optional) A json array of json objects\n"
            "     [\n"
            "       {\n"
            "         \"address\":\"address\",  (string, required) The receiver PublicKey25519Proposition in the SC\n"
            "         \"amount\":amount         (numeric, required) The numeric amount in " + CURRENCY_UNIT + " is the value\n"
            "         \"scid\":side chain ID    (string, required) The uint256 side chain ID\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"

            "\nResult:\n"
            "\"transaction\"            (string) hex string of the transaction\n"

            "\nExamples\n"
            + HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\" \"{\\\"address\\\":0.01}\"")
            + HelpExampleRpc("createrawtransaction", "\"[{\\\"txid\\\":\\\"myid\\\",\\\"vout\\\":0}]\", \"{\\\"address\\\":0.01}\"")
            + HelpExampleRpc("createrawtransaction", "\"[]\" \"{}\" \"[{\\\"epoch_length\\\" :300}]\" \"{\\\"address\\\": \\\"myaddress\\\", \\\"amount\\\": 4.0, \\\"scid\\\": \\\"myscid\\\"}]\"")
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of 
        (UniValue::VARR)(UniValue::VOBJ)(UniValue::VARR) (UniValue::VARR));

    UniValue inputs = params[0].get_array();
    UniValue sendTo = params[1].get_obj();

    CMutableTransaction rawTx;

    AddInputsToRawObject(rawTx, inputs);
    AddOutputsToRawObject(rawTx, sendTo);

    // crosschain creation
    if (params.size() > 2 && !params[2].isNull())
    {
        UniValue sc_crs = params[2].get_array();

        if (sc_crs.size())
        {
            std::string errString;
            if (!Sidechain::AddSidechainCreationOutputs(sc_crs, rawTx, errString) )
            {
                throw JSONRPCError(RPC_TYPE_ERROR, errString);
            }
        }
    }

    // crosschain forward transfers
    if (params.size() > 3 && !params[3].isNull())
    {
        UniValue fwdtr = params[3].get_array();

        if (fwdtr.size())
        {
            std::string errString;
            if (!Sidechain::AddSidechainForwardOutputs(fwdtr, rawTx, errString) )
            {
                throw JSONRPCError(RPC_TYPE_ERROR, errString);
            }
        }
    }

    return EncodeHexTx(rawTx);
}

UniValue decoderawtransaction(const UniValue& params, bool fHelp)
{   
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "decoderawtransaction \"hexstring\"\n"
            "\nReturn a JSON object representing the serialized, hex-encoded transaction.\n"

            "\nArguments:\n"
            "1. \"hex\"      (string, required) The transaction hex string\n"

            "\nResult:\n"
            "{\n"
            "  \"txid\" : \"id\",        (string) The transaction id\n"
            "  \"version\" : n,          (numeric) The version\n"
            "  \"locktime\" : ttt,       (numeric) The lock time\n"
            "  \"vin\" : [               (array of json objects)\n"
            "     {\n"
            "       \"txid\": \"id\",    (string) The transaction id\n"
            "       \"vout\": n,         (numeric) The output number\n"
            "       \"scriptSig\": {     (json object) The script\n"
            "         \"asm\": \"asm\",  (string) asm\n"
            "         \"hex\": \"hex\"   (string) hex\n"
            "       },\n"
            "       \"sequence\": n     (numeric) The script sequence number\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vout\" : [             (array of json objects)\n"
            "     {\n"
            "       \"value\" : x.xxx,            (numeric) The value in " + CURRENCY_UNIT + "\n"
            "       \"n\" : n,                    (numeric) index\n"
            "       \"scriptPubKey\" : {          (json object)\n"
            "         \"asm\" : \"asm\",          (string) the asm\n"
            "         \"hex\" : \"hex\",          (string) the hex\n"
            "         \"reqSigs\" : n,            (numeric) The required sigs\n"
            "         \"type\" : \"pubkeyhash\",  (string) The type, eg 'pubkeyhash'\n"
            "         \"addresses\" : [           (json array of string)\n"
            "           \"t12tvKAXCxZjSmdNbao16dKXC8tRWfcF5oc\"   (string) Horizen address\n"
            "           ,...\n"
            "         ]\n"
            "       }\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "  \"vjoinsplit\" : [        (array of json objects, only for version >= 2)\n"
            "     {\n"
            "       \"vpub_old\" : x.xxx,         (numeric) public input value in " + CURRENCY_UNIT + "\n"
            "       \"vpub_new\" : x.xxx,         (numeric) public output value in " + CURRENCY_UNIT + "\n"
            "       \"anchor\" : \"hex\",         (string) the anchor\n"
            "       \"nullifiers\" : [            (json array of string)\n"
            "         \"hex\"                     (string) input note nullifier\n"
            "         ,...\n"
            "       ],\n"
            "       \"commitments\" : [           (json array of string)\n"
            "         \"hex\"                     (string) output note commitment\n"
            "         ,...\n"
            "       ],\n"
            "       \"onetimePubKey\" : \"hex\",  (string) the onetime public key used to encrypt the ciphertexts\n"
            "       \"randomSeed\" : \"hex\",     (string) the random seed\n"
            "       \"macs\" : [                  (json array of string)\n"
            "         \"hex\"                     (string) input note MAC\n"
            "         ,...\n"
            "       ],\n"
            "       \"proof\" : \"hex\",          (string) the zero-knowledge proof\n"
            "       \"ciphertexts\" : [           (json array of string)\n"
            "         \"hex\"                     (string) output note ciphertext\n"
            "         ,...\n"
            "       ]\n"
            "     }\n"
            "     ,...\n"
            "  ],\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("decoderawtransaction", "\"hexstring\"")
            + HelpExampleRpc("decoderawtransaction", "\"hexstring\"")
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR));

    CTransaction tx;

    if (!DecodeHexTx(tx, params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");

    UniValue result(UniValue::VOBJ);
    TxToJSON(tx, uint256(), result);

    return result;
}

UniValue createrawcertificate(const UniValue& params, bool fHelp)
{   
    if (fHelp || params.size() != 4)
        throw runtime_error(
            "createrawcertificate [{\"txid\":\"id\",\"vout\":n},...] {\"address\":amount,...} {\"pubkeyhash\":amount,...} {\"scid\":\"id\", \"withdrawalEpochNumber\":n, \"quality\":n, \"endEpochBlockHash\":\"blockHash\", \"scProof\":\"scProof\"})\n"
            "\nCreate a SC certificate spending the given inputs, sending to the given addresses and transferring funds from the specified SC to the given pubkey hash list.\n"
            "Returns hex-encoded raw certificate.\n"
            "It is not stored in the wallet or transmitted to the network.\n"

            "\nArguments:\n"
            "1. \"transactions\"           (string, required) A json array of json objects. Can be an empty array\n"
            "     [\n"                     
            "       {\n"                   
            "         \"txid\":\"id\",                 (string, required) The transaction id\n"
            "         \"vout\":n                     (numeric, required) The output number\n"
            "       }\n"                   
            "       ,...\n"                
            "     ]\n"                     
            "2. \"vout addresses\"         (string, required) a json object with addresses as keys and amounts as values. Can also be an empty obj\n"
            "    {\n"                      
            "      \"address\": x.xxx                (numeric, required) The key is the Horizen address, the value is the " + CURRENCY_UNIT + " amount\n"
            "      ,...\n"                            
            "    }\n"                      
            "3. \"backward addresses\"     (string, required) A json object with pubkeyhash as keys and amounts as values. Can be an empty obj if no amounts are trasferred (empty certificate)\n"
            "    {\n"                               
            "      \"pubkeyhash\": x.xxx             (numeric, required) The public key hash corresponding to a Horizen address and the " + CURRENCY_UNIT + " amount to send to\n"
            "      ,...\n"                                  
            "    }\n"                               
            "4. \"certificate parameters\" (string, required) A json object with a list of key/values\n"
            "    {\n"
            "      \"scid\":\"id\",                    (string, required) The side chain id\n"
            "      \"withdrawalEpochNumber\":n       (numeric, required) The epoch number this certificate refers to\n"
            "      \"quality\":n                     (numeric, required) A positive number specifying the quality of this withdrawal certificate. \n"
            "      \"endEpochBlockHash\":\"blockHash\" (string, required) The block hash determining the end of the referenced epoch\n"
            "      \"scProof\":\"scProof\"             (string, required) SNARK proof whose verification key wCertVk was set upon sidechain registration. Its size must be " + strprintf("%d", SC_PROOF_SIZE) + "bytes \n"
            "    }\n"
            "\nResult:\n"
            "\"certificate\" (string) hex string of the certificate\n"

            "\nExamples\n"
            + HelpExampleCli("createrawcertificate",
                "\'[{\"txid\":\"7e3caf89f5f56fa7466f41d869d48c17ed8148a5fc6cc4c5923664dd2e667afe\", \"vout\": 0}]\' "
                "\'{\"ztmDWqXc2ZaMDGMhsgnVEmPKGLhi5GhsQok\":10.0}\' \'{\"fde10bda830e1d8590ca8bb8da8444cad953a852\":0.1}\' "
                "\'{\"scid\":\"02c5e79e8090c32e01e2a8636bfee933fd63c0cc15a78f0888cdf2c25b4a5e5f\", \"withdrawalEpochNumber\":3, \"quality\":10, \"endEpochBlockHash\":\"0555e4e775ce3cf79d2c15b8981df46c7448e0b408ad0a7c30c043fe5341c04e\", \"scProof\": \"abcd..ef\"}\'"
                )
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of 
        (UniValue::VARR)(UniValue::VOBJ) (UniValue::VOBJ)(UniValue::VOBJ));

    UniValue inputs          = params[0].get_array();
    UniValue standardOutputs = params[1].get_obj();
    UniValue backwardOutputs = params[2].get_obj();
    UniValue cert_params     = params[3].get_obj();

    CMutableScCertificate rawCert;
    rawCert.nVersion = SC_CERT_VERSION;

    // inputs
    AddInputsToRawObject(rawCert, inputs);

    // outputs: there should be just one of them accounting for the change, but we do not prevent a vector of outputs
    AddOutputsToRawObject(rawCert, standardOutputs);

    // backward transfer outputs
    set<CBitcoinAddress> setAddress;
    vector<string> addrList = backwardOutputs.getKeys();
    BOOST_FOREACH(const string& name_, addrList)
    {
        uint160 pkeyValue;
        pkeyValue.SetHex(name_);

        CKeyID keyID(pkeyValue);
        CBitcoinAddress address(keyID);
        if (!address.IsValid())
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, string("Invalid Horizen address: ")+name_);

        if (setAddress.count(address))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, duplicated address: ")+name_);
        setAddress.insert(address);

        CScript scriptPubKey = GetScriptForDestination(address.Get(), false);
        CAmount nAmount = AmountFromValue(backwardOutputs[name_]);

        rawCert.addBwt(CTxOut(nAmount, scriptPubKey));
    }

    if (!cert_params.isObject())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected object");

    // keywords set in cmd
    std::set<std::string> setKeyArgs;

    // valid input keywords for certificate data
    static const std::set<std::string> validKeyArgs = {"scid", "withdrawalEpochNumber", "quality", "endEpochBlockHash", "scProof"};

    // sanity check, report error if unknown/duplicate key-value pairs
    for (const string& s : cert_params.getKeys())
    {
        if (!validKeyArgs.count(s))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Invalid parameter, unknown key: ") + s);

        if (setKeyArgs.count(s))
            throw JSONRPCError(RPC_INVALID_PARAMETER, string("Duplicate key in input: ") + s);

        setKeyArgs.insert(s);
    }

    uint256 scId;
    if (setKeyArgs.count("scid"))
    {
        string inputString = find_value(cert_params, "scid").get_str();
        scId.SetHex(inputString);
    }
    else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing mandatory parameter in input: \"scid\"" );
    }

    int withdrawalEpochNumber = -1;
    if (setKeyArgs.count("withdrawalEpochNumber"))
    {
        withdrawalEpochNumber = find_value(cert_params, "withdrawalEpochNumber").get_int();
    }
    else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing mandatory parameter in input: \"withdrawalEpochNumber\"" );
    }

    int64_t quality;
    if (setKeyArgs.count("quality"))
    {
        quality = find_value(cert_params, "quality").get_int64();
        if (quality < 0)
        {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter \"quality\": must be a positive number");
        }
    }
    else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing mandatory parameter in input: \"quality\"" );
    }

    uint256 endEpochBlockHash;
    if (setKeyArgs.count("endEpochBlockHash"))
    {
        string inputString = find_value(cert_params, "endEpochBlockHash").get_str();
        endEpochBlockHash.SetHex(inputString);
    }
    else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing mandatory parameter in input: \"endEpochBlockHash\"" );
    }

    if (setKeyArgs.count("scProof"))
    {
        string inputString = find_value(cert_params, "scProof").get_str();
        std::string error;
        std::vector<unsigned char> scProofVec;
        if (!Sidechain::AddScData(inputString, scProofVec, SC_PROOF_SIZE, true, error))
            throw JSONRPCError(RPC_TYPE_ERROR, string("scProof: ") + error);

        libzendoomc::ScProof scProof(scProofVec);
        if (!libzendoomc::IsValidScProof(scProof))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid cert \"scProof\"");
        
        rawCert.scProof = scProof;
    }
    else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing mandatory parameter in input: \"scProof\"" );
    }

    rawCert.scId = scId;
    rawCert.epochNumber = withdrawalEpochNumber;
    rawCert.quality = quality;
    rawCert.endEpochBlockHash = endEpochBlockHash;

    return EncodeHexCert(rawCert);
}


UniValue decoderawcertificate(const UniValue& params, bool fHelp)
{   
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "decoderawcertificate \"hexstring\"\n"

            "\nExamples:\n"
            + HelpExampleCli("decoderawcertificate", "\"hexstring\"")
            + HelpExampleRpc("decoderawcertificate", "\"hexstring\"")
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR));

    CScCertificate cert;

    if (!DecodeHexCert(cert, params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");

    UniValue result(UniValue::VOBJ);
    CertToJSON(cert, uint256(), result);

    return result;
}

UniValue decodescript(const UniValue& params, bool fHelp)
{   
    if (fHelp || params.size() != 1)
        throw runtime_error(
            "decodescript \"hex\"\n"
            "\nDecode a hex-encoded script.\n"
            "\nArguments:\n"
            "1. \"hex\"     (string) the hex encoded script\n"
            "\nResult:\n"
            "{\n"
            "  \"asm\":\"asm\",   (string) Script public key\n"
            "  \"hex\":\"hex\",   (string) hex encoded public key\n"
            "  \"type\":\"type\", (string) The output type\n"
            "  \"reqSigs\": n,    (numeric) The required signatures\n"
            "  \"addresses\": [   (json array of string)\n"
            "     \"address\"     (string) Zen address\n"
            "     ,...\n"
            "  ],\n"
            "  \"p2sh\",\"address\" (string) script address\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("decodescript", "\"hexstring\"")
            + HelpExampleRpc("decodescript", "\"hexstring\"")
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR));

    UniValue r(UniValue::VOBJ);
    CScript script;
    if (params[0].get_str().size() > 0){
        vector<unsigned char> scriptData(ParseHexV(params[0], "argument"));
        script = CScript(scriptData.begin(), scriptData.end());
    } else {
        // Empty scripts are valid
    }
    ScriptPubKeyToJSON(script, r, false);

    r.push_back(Pair("p2sh", CBitcoinAddress(CScriptID(script)).ToString()));
    return r;
}

/** Pushes a JSON object for script verification or signing errors to vErrorsRet. */
static void TxInErrorToJSON(const CTxIn& txin, UniValue& vErrorsRet, const std::string& strMessage)
{
    UniValue entry(UniValue::VOBJ);
    entry.push_back(Pair("txid", txin.prevout.hash.ToString()));
    entry.push_back(Pair("vout", (uint64_t)txin.prevout.n));
    entry.push_back(Pair("scriptSig", HexStr(txin.scriptSig.begin(), txin.scriptSig.end())));
    entry.push_back(Pair("sequence", (uint64_t)txin.nSequence));
    entry.push_back(Pair("error", strMessage));
    vErrorsRet.push_back(entry);
}

UniValue signrawcertificate(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "signrawcertificate \"hexstring\" ([\"privatekey1\",...] )\n"
            "\nSign inputs for raw certificate (serialized, hex-encoded).\n"
            "The second optional argument (may be null) is an array of base58-encoded private\n"
            "keys that, if given, will be the only keys used to sign the transaction.\n"
#ifdef ENABLE_WALLET
            + HelpRequiringPassphrase() + "\n"
#endif

            "\nArguments:\n"
            "1. \"hexstring\"     (string, required) The transaction hex string\n"
            "2. \"privatekeys\"     (string, optional) A json array of base58-encoded private keys for signing\n"
            "    [                  (json array of strings, or 'null' if none provided)\n"
            "      \"privatekey\"   (string) private key in base58-encoding\n"
            "      ,...\n"
            "    ]\n"

            "\nResult:\n"
            "{\n"
            "  \"hex\" : \"value\",           (string) The hex-encoded raw transaction with signature(s)\n"
            "  \"complete\" : true|false,   (boolean) If the transaction has a complete set of signatures\n"
            "  \"errors\" : [                 (json array of objects) Script verification errors (if there are any)\n"
            "    {\n"
            "      \"txid\" : \"hash\",           (string) The hash of the referenced, previous input transaction\n"
            "      \"vout\" : n,                (numeric) The index of the output to spent and used as input\n"
            "      \"scriptSig\" : \"hex\",       (string) The hex-encoded signature script\n"
            "      \"sequence\" : n,            (numeric) Script sequence number\n"
            "      \"error\" : \"text\"           (string) Verification or signing error related to the input\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("signrawtransaction", "\"myhex\"")
            + HelpExampleRpc("signrawtransaction", "\"myhex\"")
        );

#ifdef ENABLE_WALLET
    LOCK2(cs_main, pwalletMain ? &pwalletMain->cs_wallet : NULL);
#else
    LOCK(cs_main);
#endif
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)(UniValue::VARR), true);

    vector<unsigned char> certData(ParseHexV(params[0], "argument 1"));
    CDataStream ssData(certData, SER_NETWORK, PROTOCOL_VERSION);

    if (ssData.empty())
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Missing input certificate");

    CMutableScCertificate certVariants;
    try {
        ssData >> certVariants;
    }
    catch (const std::exception&) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Cert decode failed");
    }

    if (!ssData.empty()) {
        // just one and only one certificate expected
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, strprintf("Found %d extra byte%safter certificate",
            ssData.size(), ssData.size()>1?"s ":" "));
    }

    // mergedCert will end up with all the signatures; it
    // starts as a clone of the rawcert:
    CMutableScCertificate mergedCert(certVariants);

    // Fetch previous transactions (inputs):
    CCoinsView viewDummy;
    CCoinsViewCache view(&viewDummy);
    {
        LOCK(mempool.cs);
        CCoinsViewCache &viewChain = *pcoinsTip;
        CCoinsViewMemPool viewMempool(&viewChain, mempool);
        view.SetBackend(viewMempool); // temporarily switch cache backend to db+mempool view

        BOOST_FOREACH(const CTxIn& txin, mergedCert.vin) {
            const uint256& prevHash = txin.prevout.hash;
            CCoins coins;
            view.AccessCoins(prevHash); // this is certainly allowed to fail
        }

        view.SetBackend(viewDummy); // switch back to avoid locking mempool for too long
    }

    bool fGivenKeys = false;
    CBasicKeyStore tempKeystore;
    if (params.size() > 1 && !params[1].isNull()) {
        fGivenKeys = true;
        UniValue keys = params[1].get_array();
        for (size_t idx = 0; idx < keys.size(); idx++) {
            UniValue k = keys[idx];
            CBitcoinSecret vchSecret;
            bool fGood = vchSecret.SetString(k.get_str());
            if (!fGood)
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key");
            CKey key = vchSecret.GetKey();
            if (!key.IsValid())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Private key outside allowed range");
            tempKeystore.AddKey(key);
        }
    }

    #ifdef ENABLE_WALLET
        EnsureWalletIsUnlocked();
        const CKeyStore& keystore = ((fGivenKeys || !pwalletMain) ? tempKeystore : *pwalletMain);
    #else
        const CKeyStore& keystore = tempKeystore;
    #endif

    int nHashType = SIGHASH_ALL;

    // Script verification errors
    UniValue vErrors(UniValue::VARR);

    // Sign what we can:
    for (unsigned int i = 0; i < mergedCert.vin.size(); i++) {
        CTxIn& txin = mergedCert.vin[i];
        const CCoins* coins = view.AccessCoins(txin.prevout.hash);
        if (coins == NULL || !coins->IsAvailable(txin.prevout.n)) {
            TxInErrorToJSON(txin, vErrors, "Input not found or already spent");
            continue;
        }
        const CScript& prevPubKey = coins->vout[txin.prevout.n].scriptPubKey;

        txin.scriptSig.clear();
        SignSignature(keystore, prevPubKey, mergedCert, i, nHashType);

        ScriptError serror = SCRIPT_ERR_OK;
        if (!VerifyScript(
                txin.scriptSig, prevPubKey, STANDARD_NONCONTEXTUAL_SCRIPT_VERIFY_FLAGS,
                MutableCertificateSignatureChecker(&mergedCert, i),
                &serror
           ))
        {
            TxInErrorToJSON(txin, vErrors, ScriptErrorString(serror));
        }
    }
    bool fComplete = vErrors.empty();

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hex", EncodeHexCert(CScCertificate(mergedCert))));
    result.push_back(Pair("complete", fComplete));
    if (!vErrors.empty()) {
        result.push_back(Pair("errors", vErrors));
    }

    return result;
}

UniValue signrawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 4)
        throw runtime_error(
            "signrawtransaction \"hexstring\" ( [{\"txid\":\"id\",\"vout\":n,\"scriptPubKey\":\"hex\",\"redeemScript\":\"hex\"},...] [\"privatekey1\",...] sighashtype )\n"
            "\nSign inputs for raw transaction (serialized, hex-encoded).\n"
            "The second optional argument (may be null) is an array of previous transaction outputs that\n"
            "this transaction depends on but may not yet be in the block chain.\n"
            "The third optional argument (may be null) is an array of base58-encoded private\n"
            "keys that, if given, will be the only keys used to sign the transaction.\n"
#ifdef ENABLE_WALLET
            + HelpRequiringPassphrase() + "\n"
#endif

            "\nArguments:\n"
            "1. \"hexstring\"     (string, required) The transaction hex string\n"
            "2. \"prevtxs\"       (string, optional) An json array of previous dependent transaction outputs\n"
            "     [               (json array of json objects, or 'null' if none provided)\n"
            "       {\n"
            "         \"txid\":\"id\",             (string, required) The transaction id\n"
            "         \"vout\":n,                  (numeric, required) The output number\n"
            "         \"scriptPubKey\": \"hex\",   (string, required) script key\n"
            "         \"redeemScript\": \"hex\"    (string, required for P2SH) redeem script\n"
            "       }\n"
            "       ,...\n"
            "    ]\n"
            "3. \"privatekeys\"     (string, optional) A json array of base58-encoded private keys for signing\n"
            "    [                  (json array of strings, or 'null' if none provided)\n"
            "      \"privatekey\"   (string) private key in base58-encoding\n"
            "      ,...\n"
            "    ]\n"
            "4. \"sighashtype\"     (string, optional, default=ALL) The signature hash type. Must be one of\n"
            "       \"ALL\"\n"
            "       \"NONE\"\n"
            "       \"SINGLE\"\n"
            "       \"ALL|ANYONECANPAY\"\n"
            "       \"NONE|ANYONECANPAY\"\n"
            "       \"SINGLE|ANYONECANPAY\"\n"

            "\nResult:\n"
            "{\n"
            "  \"hex\" : \"value\",           (string) The hex-encoded raw transaction with signature(s)\n"
            "  \"complete\" : true|false,   (boolean) If the transaction has a complete set of signatures\n"
            "  \"errors\" : [                 (json array of objects) Script verification errors (if there are any)\n"
            "    {\n"
            "      \"txid\" : \"hash\",           (string) The hash of the referenced, previous transaction\n"
            "      \"vout\" : n,                (numeric) The index of the output to spent and used as input\n"
            "      \"scriptSig\" : \"hex\",       (string) The hex-encoded signature script\n"
            "      \"sequence\" : n,            (numeric) Script sequence number\n"
            "      \"error\" : \"text\"           (string) Verification or signing error related to the input\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("signrawtransaction", "\"myhex\"")
            + HelpExampleRpc("signrawtransaction", "\"myhex\"")
        );

#ifdef ENABLE_WALLET
    LOCK2(cs_main, pwalletMain ? &pwalletMain->cs_wallet : NULL);
#else
    LOCK(cs_main);
#endif
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)(UniValue::VARR)(UniValue::VARR)(UniValue::VSTR), true);

    vector<unsigned char> txData(ParseHexV(params[0], "argument 1"));
    CDataStream ssData(txData, SER_NETWORK, PROTOCOL_VERSION);
    vector<CMutableTransaction> txVariants;
    while (!ssData.empty()) {
        try {
            CMutableTransaction tx;
            ssData >> tx;
            txVariants.push_back(tx);
        }
        catch (const std::exception&) {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
        }
    }

    if (txVariants.empty())
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Missing transaction");

    // mergedTx will end up with all the signatures; it
    // starts as a clone of the rawtx:
    CMutableTransaction mergedTx(txVariants[0]);

    // Fetch previous transactions (inputs):
    CCoinsView viewDummy;
    CCoinsViewCache view(&viewDummy);
    {
        LOCK(mempool.cs);
        CCoinsViewCache &viewChain = *pcoinsTip;
        CCoinsViewMemPool viewMempool(&viewChain, mempool);
        view.SetBackend(viewMempool); // temporarily switch cache backend to db+mempool view

        BOOST_FOREACH(const CTxIn& txin, mergedTx.vin) {
            const uint256& prevHash = txin.prevout.hash;
            CCoins coins;
            view.AccessCoins(prevHash); // this is certainly allowed to fail
        }

        view.SetBackend(viewDummy); // switch back to avoid locking mempool for too long
    }

    bool fGivenKeys = false;
    CBasicKeyStore tempKeystore;
    if (params.size() > 2 && !params[2].isNull()) {
        fGivenKeys = true;
        UniValue keys = params[2].get_array();
        for (size_t idx = 0; idx < keys.size(); idx++) {
            UniValue k = keys[idx];
            CBitcoinSecret vchSecret;
            bool fGood = vchSecret.SetString(k.get_str());
            if (!fGood)
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key");
            CKey key = vchSecret.GetKey();
            if (!key.IsValid())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Private key outside allowed range");
            tempKeystore.AddKey(key);
        }
    }
#ifdef ENABLE_WALLET
    else if (pwalletMain)
        EnsureWalletIsUnlocked();
#endif

    // Add previous txouts given in the RPC call:
    if (params.size() > 1 && !params[1].isNull()) {
        UniValue prevTxs = params[1].get_array();
        for (size_t idx = 0; idx < prevTxs.size(); idx++) {
            const UniValue& p = prevTxs[idx];
            if (!p.isObject())
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "expected object with {\"txid'\",\"vout\",\"scriptPubKey\"}");

            UniValue prevOut = p.get_obj();

            RPCTypeCheckObj(prevOut, boost::assign::map_list_of("txid", UniValue::VSTR)("vout", UniValue::VNUM)("scriptPubKey", UniValue::VSTR));

            uint256 txid = ParseHashO(prevOut, "txid");

            int nOut = find_value(prevOut, "vout").get_int();
            if (nOut < 0)
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "vout must be positive");

            vector<unsigned char> pkData(ParseHexO(prevOut, "scriptPubKey"));
            CScript scriptPubKey(pkData.begin(), pkData.end());

            {
                CCoinsModifier coins = view.ModifyCoins(txid);
                if (coins->IsAvailable(nOut) && coins->vout[nOut].scriptPubKey != scriptPubKey) {
                    string err("Previous output scriptPubKey mismatch:\n");
                    err = err + coins->vout[nOut].scriptPubKey.ToString() + "\nvs:\n"+
                        scriptPubKey.ToString();
                    throw JSONRPCError(RPC_DESERIALIZATION_ERROR, err);
                }
                if ((unsigned int)nOut >= coins->vout.size())
                    coins->vout.resize(nOut+1);
                coins->vout[nOut].scriptPubKey = scriptPubKey;
                coins->vout[nOut].nValue = 0; // we don't know the actual output value
            }

            // if redeemScript given and not using the local wallet (private keys
            // given), add redeemScript to the tempKeystore so it can be signed:
            if (fGivenKeys && scriptPubKey.IsPayToScriptHash()) {
                RPCTypeCheckObj(prevOut, boost::assign::map_list_of("txid", UniValue::VSTR)("vout", UniValue::VNUM)("scriptPubKey", UniValue::VSTR)("redeemScript",UniValue::VSTR));
                UniValue v = find_value(prevOut, "redeemScript");
                if (!v.isNull()) {
                    vector<unsigned char> rsData(ParseHexV(v, "redeemScript"));
                    CScript redeemScript(rsData.begin(), rsData.end());
                    tempKeystore.AddCScript(redeemScript);
                }
            }
        }
    }

#ifdef ENABLE_WALLET
    const CKeyStore& keystore = ((fGivenKeys || !pwalletMain) ? tempKeystore : *pwalletMain);
#else
    const CKeyStore& keystore = tempKeystore;
#endif

    int nHashType = SIGHASH_ALL;
    if (params.size() > 3 && !params[3].isNull()) {
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
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid sighash param");
    }

    bool fHashSingle = ((nHashType & ~SIGHASH_ANYONECANPAY) == SIGHASH_SINGLE);

    // Script verification errors
    UniValue vErrors(UniValue::VARR);

    // Sign what we can:
    for (unsigned int i = 0; i < mergedTx.vin.size(); i++) {
        CTxIn& txin = mergedTx.vin[i];
        const CCoins* coins = view.AccessCoins(txin.prevout.hash);
        if (coins == NULL || !coins->IsAvailable(txin.prevout.n)) {
            TxInErrorToJSON(txin, vErrors, "Input not found or already spent");
            continue;
        }
        const CScript& prevPubKey = coins->vout[txin.prevout.n].scriptPubKey;

        txin.scriptSig.clear();
        // Only sign SIGHASH_SINGLE if there's a corresponding output:
        if (!fHashSingle || (i < mergedTx.getVout().size()))
            SignSignature(keystore, prevPubKey, mergedTx, i, nHashType);

        // ... and merge in other signatures:
        BOOST_FOREACH(const CMutableTransaction& txv, txVariants) {
            txin.scriptSig = CombineSignatures(prevPubKey, mergedTx, i, txin.scriptSig, txv.vin[i].scriptSig);
        }
        ScriptError serror = SCRIPT_ERR_OK;
        if (!VerifyScript(txin.scriptSig, prevPubKey, STANDARD_NONCONTEXTUAL_SCRIPT_VERIFY_FLAGS, MutableTransactionSignatureChecker(&mergedTx, i), &serror)) {
            TxInErrorToJSON(txin, vErrors, ScriptErrorString(serror));
        }
    }
    bool fComplete = vErrors.empty();

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hex", EncodeHexTx(CTransaction(mergedTx))));
    result.push_back(Pair("complete", fComplete));
    if (!vErrors.empty()) {
        result.push_back(Pair("errors", vErrors));
    }

    return result;
}

UniValue sendrawtransaction(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "sendrawtransaction \"hexstring\" ( allowhighfees )\n"
            "\nSubmits raw transaction (serialized, hex-encoded) to local node and network.\n"
            "\nAlso see createrawtransaction and signrawtransaction calls.\n"
            "\nArguments:\n"
            "1. \"hexstring\"    (string, required) The hex string of the raw transaction)\n"
            "2. allowhighfees    (boolean, optional, default=false) Allow high fees\n"
            "\nResult:\n"
            "\"hex\"             (string) The transaction hash in hex\n"
            "\nExamples:\n"
            "\nCreate a transaction\n"
            + HelpExampleCli("createrawtransaction", "\"[{\\\"txid\\\" : \\\"mytxid\\\",\\\"vout\\\":0}]\" \"{\\\"myaddress\\\":0.01}\"") +
            "Sign the transaction, and get back the hex\n"
            + HelpExampleCli("signrawtransaction", "\"myhex\"") +
            "\nSend the transaction (signed hex)\n"
            + HelpExampleCli("sendrawtransaction", "\"signedhex\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("sendrawtransaction", "\"signedhex\"")
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)(UniValue::VBOOL));

    // parse hex string from parameter
    CTransaction tx;
    if (!DecodeHexTx(tx, params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    uint256 hashTx = tx.GetHash();

    bool fOverrideFees = false;
    if (params.size() > 1)
        fOverrideFees = params[1].get_bool();

    CCoinsViewCache &view = *pcoinsTip;
    const CCoins* existingCoins = view.AccessCoins(hashTx);
    bool fHaveMempool = mempool.exists(hashTx);
    bool fHaveChain = existingCoins && existingCoins->nHeight < 1000000000;
    if (!fHaveMempool && !fHaveChain) {
        // push to local node and sync with wallets
        CValidationState state;
        bool fMissingInputs;
        if (!AcceptTxToMemoryPool(mempool, state, tx, false, &fMissingInputs, !fOverrideFees)) {
            if (state.IsInvalid()) {
                throw JSONRPCError(RPC_TRANSACTION_REJECTED, strprintf("%i: %s", state.GetRejectCode(), state.GetRejectReason()));
            } else {
                if (fMissingInputs) {
                    throw JSONRPCError(RPC_TRANSACTION_ERROR, "Missing inputs");
                }
                throw JSONRPCError(RPC_TRANSACTION_ERROR, state.GetRejectReason());
            }
        }
    } else if (fHaveChain) {
        throw JSONRPCError(RPC_TRANSACTION_ALREADY_IN_CHAIN, "transaction already in block chain");
    }
    tx.Relay();

    return hashTx.GetHex();
}

UniValue sendrawcertificate(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() < 1 || params.size() > 2)
        throw runtime_error(
            "sendrawcertificate \"hexstring\" ( allowhighfees )\n"
            "\nSubmits raw certificate (serialized, hex-encoded) to local node and network.\n"
            "\nArguments:\n"
            "1. \"hexstring\"    (string, required) The hex string of the raw transaction)\n"
            "2. allowhighfees    (boolean, optional, default=false) Allow high fees\n"
            "\nResult:\n"
            "\"hex\"             (string) The transaction hash in hex\n"
            "\nExamples:\n"
            + HelpExampleCli("sendrawcertificate", "\"hex\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("sendrawcertificate", "\"hex\"")
        );

    LOCK(cs_main);
    RPCTypeCheck(params, boost::assign::list_of(UniValue::VSTR)(UniValue::VBOOL));

    // parse hex string from parameter
    CScCertificate cert;
    if (!DecodeHexCert(cert, params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Certificate decode failed");
    const uint256& hashCertificate = cert.GetHash();

    bool fOverrideFees = false;
    if (params.size() > 1)
    {
        fOverrideFees = params[1].get_bool();
    }

    // check that we do not have it already somewhere
    CCoinsViewCache &view = *pcoinsTip;
    const CCoins* existingCoins = view.AccessCoins(hashCertificate);

    bool fHaveChain = existingCoins;
    bool fHaveMempool = mempool.existsCert(hashCertificate);

    if (!fHaveMempool && !fHaveChain)
    {
        // push to local node and sync with wallets
        CValidationState state;
        bool fMissingInputs;
        if (!AcceptCertificateToMemoryPool(mempool, state, cert, false, &fMissingInputs, !fOverrideFees))
        {
            LogPrintf("%s():%d - cert[%s] not accepted in mempool\n", __func__, __LINE__, hashCertificate.ToString());
            if (state.IsInvalid())
            {
                throw JSONRPCError(RPC_TRANSACTION_REJECTED, strprintf("%i: %s", state.GetRejectCode(), state.GetRejectReason()));
            }
            else
            {
                if (fMissingInputs)
                {
                    throw JSONRPCError(RPC_TRANSACTION_ERROR, "Missing inputs");
                }
                throw JSONRPCError(RPC_TRANSACTION_ERROR, "certificate not accepted to mempool");
            }
        }
    }
    else if (fHaveChain)
    {
        throw JSONRPCError(RPC_TRANSACTION_ALREADY_IN_CHAIN, "certificate already in block chain");
    }
    else
    {
        LogPrint("cert", "%s():%d - cert[%s] is already in mempool, just realying it\n", __func__, __LINE__, hashCertificate.ToString());
    }

    LogPrint("cert", "%s():%d - relaying certificate [%s]\n", __func__, __LINE__, hashCertificate.ToString());
    cert.Relay();

    return hashCertificate.GetHex();
}
