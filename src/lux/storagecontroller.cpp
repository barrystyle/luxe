#include "storagecontroller.h"

#include <fstream>
#include <iostream>
#include <boost/filesystem.hpp>

#include "main.h"
#include "streams.h"
#include "util.h"
#include "replicabuilder.h"
#include "serialize.h"
#include "merkler.h"

std::unique_ptr<StorageController> storageController;

struct ReplicaStream
{
    const uint64_t BUFFER_SIZE = 4 * 1024;
    std::fstream filestream;
    uint256 currentOrderHash;
    uint256 merkleRootHash;

    ADD_SERIALIZE_METHODS;
    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action, int nType, int nVersion) {
        std::vector<char> buf(BUFFER_SIZE);

        const auto *order = storageController->GetAnnounce(currentOrderHash);
        const auto fileSize = GetCryptoReplicaSize(order->fileSize);
        if (order) {
            if (!ser_action.ForRead()) {
                READWRITE(currentOrderHash);
                READWRITE(merkleRootHash);
                for (auto i = 0u; i < fileSize;) {
                    uint64_t n = std::min(BUFFER_SIZE, fileSize - i);
                    buf.resize(n);
                    filestream.read(&buf[0], n);  // TODO: change to loop of readsome
                    if (buf.empty()) {
                        break;
                    }
                    READWRITE(buf);
                    i += buf.size();
                }
            } else {
                READWRITE(currentOrderHash);
                READWRITE(merkleRootHash);
                for (auto i = 0u; i < fileSize;) {
                    READWRITE(buf);
                    filestream.write(&buf[0], buf.size());
                    i += buf.size();
                }
            }
        }
    }
};


#if OPENSSL_VERSION_NUMBER < 0x10100005L
static void RSA_get0_key(const RSA *r, const BIGNUM **n, const BIGNUM **e, const BIGNUM **d)
{
    if(n != NULL)
        *n = r->n;

    if(e != NULL)
        *e = r->e;

    if(d != NULL)
        *d = r->d;
}
#endif

void StorageController::InitStorages(const boost::filesystem::path &dataDir, const boost::filesystem::path &tempDataDir)
{
    namespace fs = boost::filesystem;

    if (!fs::exists(dataDir)) {
        fs::create_directories(dataDir);
    }
    storageHeap.AddChunk(dataDir, DEFAULT_STORAGE_SIZE);
    if (!fs::exists(tempDataDir)) {
        fs::create_directories(tempDataDir);
    }
    tempStorageHeap.AddChunk(tempDataDir, DEFAULT_STORAGE_SIZE);
}

void StorageController::ProcessStorageMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, bool& isStorageCommand)
{
    namespace fs = boost::filesystem;

    if (strCommand == "dfsannounce") {
        isStorageCommand = true;
        StorageOrder order;
        vRecv >> order;

        uint256 hash = order.GetHash();
        if (GetAnnounce(hash) == nullptr) {
            AnnounceOrder(order); // TODO: Is need remove "pfrom" node from announcement? (SS)
            if (storageHeap.MaxAllocateSize() > order.fileSize &&
                tempStorageHeap.MaxAllocateSize() > order.fileSize &&
                order.maxRate >= rate &&
                order.maxGap >= maxblocksgap) {
                StorageProposal proposal { std::time(nullptr), hash, rate, address };

                CNode* pNode = FindNode(order.address);
                if (!pNode) {
                    CAddress addr;
                    OpenNetworkConnection(addr, false, NULL, order.address.ToStringIPPort().c_str());
                    MilliSleep(500);
                    pNode = FindNode(order.address);
                }
                if (pNode) {
                    pNode->PushMessage("dfsproposal", proposal);
                } else {
                    pfrom->PushMessage("dfsproposal", proposal);
                }

            }
        }
    } else if (strCommand == "dfsproposal") {
        isStorageCommand = true;
        StorageProposal proposal;
        vRecv >> proposal;
        const auto *order = GetAnnounce(proposal.orderHash);
        if (order != nullptr) {
            std::vector<uint256> vListenProposals;
            {
                boost::lock_guard <boost::mutex> lock(mutex);
                vListenProposals = proposalsAgent.GetListenProposals();
            }

            if (std::find(vListenProposals.begin(), vListenProposals.end(), proposal.orderHash) != vListenProposals.end()) {
                if (order->maxRate > proposal.rate) {
                    boost::lock_guard <boost::mutex> lock(mutex);
                    proposalsAgent.AddProposal(proposal);
                }
            }
            CNode* pNode = FindNode(proposal.address);
            if (pNode && vNodes.size() > 5) {
                pNode->CloseSocketDisconnect();
            }
        } else {
            // DoS prevention
//            CNode* pNode = FindNode(proposal.address);
//            if (pNode) {
//                CNodeState *state = State(pNode); // CNodeState was declared in main.cpp (SS)
//                state->nMisbehavior += 10;
//            }
        }
    } else if (strCommand == "dfshandshake") {
        isStorageCommand = true;
        StorageHandshake handshake;
        vRecv >> handshake;
        const auto *order = GetAnnounce(handshake.orderHash);
        if (order != nullptr) {
            if (storageHeap.MaxAllocateSize() > order->fileSize && tempStorageHeap.MaxAllocateSize() > order->fileSize) { // TODO: Change to exist(handshake.proposalHash) (SS)
                StorageHandshake requestReplica { std::time(nullptr), handshake.orderHash, handshake.proposalHash, DEFAULT_DFS_PORT };
                mapReceivedHandshakes[handshake.orderHash] = handshake;
                CNode* pNode = FindNode(order->address);

                if (pNode) {
                    pNode->PushMessage("dfsrr", requestReplica);
                } else {
                    LogPrint("dfs", "\"dfshandshake\" message handler have not connection to order sender");
                    pfrom->PushMessage("dfsrr", requestReplica);
                }
            }
        } else {
            // DoS prevention
//            CNode* pNode = FindNode(proposal.address);
//            if (pNode) {
//                CNodeState *state = State(pNode); // CNodeState was declared in main.cpp (SS)
//                state->nMisbehavior += 10;
//            }
        }
    } else if (strCommand == "dfsrr") { // dfs request replica
        isStorageCommand = true;
        StorageHandshake handshake;
        vRecv >> handshake;
        const auto *order = GetAnnounce(handshake.orderHash);
        if (order != nullptr) {
            auto it = mapLocalFiles.find(handshake.orderHash);
            if (it != mapLocalFiles.end()) {
                mapReceivedHandshakes[handshake.orderHash] = handshake;
            } else {
                // DoS prevention
//            CNode* pNode = FindNode(proposal.address);
//            if (pNode) {
//                CNodeState *state = State(pNode); // CNodeState was declared in main.cpp (SS)
//                state->nMisbehavior += 10;
//            }
            }
        }
    } else if (strCommand == "dfssendfile") {
        isStorageCommand = true;
        ReplicaStream replicaStream;
        fs::path tempFile = tempStorageHeap.GetChunks().back()->path; // TODO: temp usage (SS)
        tempFile /= (std::to_string(std::time(nullptr)) + ".luxfs");
        replicaStream.filestream.open(tempFile.string(), std::ios::binary|std::ios::out);
        if (!replicaStream.filestream.is_open()) {
            LogPrint("dfs", "File \"%s\" cannot be opened", tempFile);
            return ;
        }
        vRecv >> replicaStream;
        replicaStream.filestream.close();
        uint256 orderHash = replicaStream.currentOrderHash;
        uint256 receivedMerkleRootHash = replicaStream.merkleRootHash;
        if (!CheckReceivedReplica(orderHash, receivedMerkleRootHash, tempFile)) {
            fs::remove(tempFile);
            return ;
        }
        const auto *order = GetAnnounce(orderHash);
        auto itHandshake = mapReceivedHandshakes.find(orderHash);
        if (itHandshake == mapReceivedHandshakes.end()) {
            LogPrint("dfs", "Handshake \"%s\" not found", orderHash.ToString());
            fs::remove(tempFile);
            return ;
        }
        auto keys = itHandshake->second.keys;
        std::shared_ptr<AllocatedFile> file = storageHeap.AllocateFile(order->fileURI, GetCryptoReplicaSize(order->fileSize));
        storageHeap.SetDecryptionKeys(file->uri, keys.rsaKey, keys.aesKey);
        fs::rename(tempFile, file->fullpath);
        LogPrint("dfs", "File \"%s\" was uploaded", order->filename);
    } else if (strCommand == "dfsping") {
        isStorageCommand = true;
        pfrom->PushMessage("dfspong", pfrom->addr);
    } else if (strCommand == "dfspong") {
        isStorageCommand = true;
        vRecv >> address;
        address.SetPort(GetListenPort());
    }
}

void StorageController::AnnounceOrder(const StorageOrder &order)
{
    uint256 hash = order.GetHash();
    mapAnnouncements[hash] = order;

    CInv inv(MSG_STORAGE_ORDER_ANNOUNCE, hash);
    vector <CInv> vInv;
    vInv.push_back(inv);

    std::vector<CNode*> vNodesCopy;
    {
        LOCK(cs_vNodes);
        vNodesCopy = vNodes;
    }
    for (auto *pNode : vNodesCopy) {
        if (!pNode) {
            continue;
        }
        if (pNode->nVersion >= ActiveProtocol()) {
            pNode->PushMessage("inv", vInv);
        }
    }
}

void StorageController::AnnounceOrder(const StorageOrder &order, const boost::filesystem::path &path)
{
    AnnounceOrder(order);
    boost::lock_guard<boost::mutex> lock(mutex);
    mapLocalFiles[order.GetHash()] = path;
    proposalsAgent.ListenProposal(order.GetHash());
}

bool StorageController::CancelOrder(const uint256 &orderHash)
{
    if (!mapAnnouncements.count(orderHash)) {
        return false;
    }

    proposalsAgent.StopListenProposal(orderHash);
    proposalsAgent.EraseOrdersProposals(orderHash);
    mapLocalFiles.erase(orderHash);
    mapAnnouncements.erase(orderHash);
    return true;
}

void StorageController::ClearOldAnnouncments(std::time_t timestamp)
{
    boost::lock_guard<boost::mutex> lock(mutex);
    for (auto &&it = mapAnnouncements.begin(); it != mapAnnouncements.end(); ) {
        StorageOrder order = it->second;
        if (order.time < timestamp) {
            std::vector<uint256> vListenProposals = proposalsAgent.GetListenProposals();
            uint256 orderHash = order.GetHash();
            if (std::find(vListenProposals.begin(), vListenProposals.end(), orderHash) != vListenProposals.end()) {
                proposalsAgent.StopListenProposal(orderHash);
            }
            std::vector<StorageProposal> vProposals = proposalsAgent.GetProposals(orderHash);
            if (vProposals.size()) {
                proposalsAgent.EraseOrdersProposals(orderHash);
            }
            mapLocalFiles.erase(orderHash);
            mapAnnouncements.erase(it++);
        } else {
            ++it;
        }
    }
}

bool StorageController::AcceptProposal(const StorageProposal &proposal)
{
    const auto *order = GetAnnounce(proposal.orderHash);
    if (order == nullptr) {
        return false;
    }
    RSA *rsa;
    DecryptionKeys keys = GenerateKeys(&rsa);
    if (!StartHandshake(proposal, keys)) {
        RSA_free(rsa);
        return false;
    }
    auto it = mapReceivedHandshakes.find(proposal.orderHash);
    for (int times = 0; times < 300 && it == mapReceivedHandshakes.end(); ++times) {
        MilliSleep(100);
        it = mapReceivedHandshakes.find(proposal.orderHash);
    }
    CNode* pNode = FindNode(proposal.address);
    if (it != mapReceivedHandshakes.end()) {
        auto itFile = mapLocalFiles.find(proposal.orderHash);
        auto pAllocatedFile = CreateReplica(itFile->second, *order, keys, rsa);
        auto pMerleTreeFile = tempStorageHeap.AllocateFile(uint256{}, pAllocatedFile->size);
        auto merkleRootHash = Merkler::ConstructMerkleTree(pAllocatedFile->fullpath, pMerleTreeFile->fullpath);
        if (pNode) {
            bool b = SendReplica(*order, merkleRootHash, pAllocatedFile, pNode);
            boost::filesystem::remove(pMerleTreeFile->fullpath);
            tempStorageHeap.FreeFile(pMerleTreeFile->uri);
            RSA_free(rsa);
            return b;
        }
    } else {
        if (pNode) {
            pNode->CloseSocketDisconnect();
        }
    }
    RSA_free(rsa);
    return false;
}

void StorageController::DecryptReplica(const uint256 &orderHash, const boost::filesystem::path &decryptedFile)
{
    namespace fs = boost::filesystem;

    const auto *order = GetAnnounce(orderHash);
    if (!order) {
        return ;
    }
    std::shared_ptr<AllocatedFile> pAllocatedFile = storageHeap.GetFile(order->fileURI);
    RSA *rsa = CreatePublicRSA(DecryptionKeys::ToString(pAllocatedFile->keys.rsaKey));
    std::ifstream filein(pAllocatedFile->fullpath.string(), std::ios::binary);
    if (!filein.is_open()) {
        LogPrint("dfs", "file %s cannot be opened", pAllocatedFile->fullpath.string());
        return ;
    }
    auto fileSize = fs::file_size(pAllocatedFile->fullpath);
    auto bytesSize = order->fileSize;
    std::ofstream outfile;
    outfile.open(decryptedFile.string().c_str(), std::ios::binary);
    uint64_t sizeBuffer = nBlockSizeRSA - 2;
    byte *buffer = new byte[sizeBuffer];
    byte *replica = new byte[nBlockSizeRSA];

    for (auto i = 0u; i < fileSize; i+= nBlockSizeRSA)
    {
        filein.read((char *)replica, nBlockSizeRSA); // TODO: change to loop of readsome

        DecryptData(replica, 0, sizeBuffer, buffer, pAllocatedFile->keys.aesKey, rsa);
        outfile.write((char *) buffer, std::min(sizeBuffer, bytesSize));
        bytesSize -= sizeBuffer;
        // TODO: check write (SS)
    }

    delete[] buffer;
    delete[] replica;
    filein.close();
    outfile.close();
    return ;
}

std::map<uint256, StorageOrder> StorageController::GetAnnouncements()
{
    return mapAnnouncements;
}

const StorageOrder *StorageController::GetAnnounce(const uint256 &hash)
{
    return mapAnnouncements.find(hash) != mapAnnouncements.end()? &(mapAnnouncements[hash]) : nullptr;
}

std::vector<std::shared_ptr<StorageChunk>> StorageController::GetChunks(bool tempChunk) const
{
    return tempChunk?
           tempStorageHeap.GetChunks() :
           storageHeap.GetChunks();
}

void StorageController::MoveChunk(size_t chunkIndex, const boost::filesystem::path &newpath, bool tempChunk)
{
    tempChunk?
    tempStorageHeap.MoveChunk(chunkIndex, newpath) :
    storageHeap.MoveChunk(chunkIndex, newpath);
}

std::vector<StorageProposal> StorageController::GetProposals(const uint256 &orderHash)
{
    boost::lock_guard<boost::mutex> lock(mutex);
    return proposalsAgent.GetProposals(orderHash);
}

StorageProposal StorageController::GetProposal(const uint256 &orderHash, const uint256 &proposalHash)
{
    return proposalsAgent.GetProposal(orderHash, proposalHash);
}

RSA* StorageController::CreatePublicRSA(const std::string &key) { // utility function
    RSA *rsa = NULL;
    BIO *keybio;
    const char* c_string = key.c_str();
    keybio = BIO_new_mem_buf((void*)c_string, -1);
    if (keybio==NULL) {
        return 0;
    }
    rsa = PEM_read_bio_RSAPublicKey(keybio, &rsa, 0, NULL);
    return rsa;
}

DecryptionKeys StorageController::GenerateKeys(RSA **rsa)
{
    const BIGNUM *rsa_n;
    // search for rsa->n > 0x0000ff...126 bytes...ff
    {
        *rsa = RSA_generate_key(nBlockSizeRSA * 8, 3, nullptr, nullptr);
        BIGNUM *minModulus = GetMinModulus();
        RSA_get0_key(*rsa, &rsa_n, nullptr, nullptr);
        while (BN_ucmp(minModulus, rsa_n) >= 0) {
            RSA_free(*rsa);
            *rsa = RSA_generate_key(nBlockSizeRSA * 8, 3, nullptr, nullptr);
        }
        BN_free(minModulus);
    }
    const char chAESKey[] = "1234567890123456"; // TODO: generate unique 16 bytes (SS)
    const AESKey aesKey(chAESKey, chAESKey + sizeof(chAESKey)/sizeof(*chAESKey));

    BIO *pub = BIO_new(BIO_s_mem());
    PEM_write_bio_RSAPublicKey(pub, *rsa);
    size_t publicKeyLength = BIO_pending(pub);
    char *rsaPubKey = new char[publicKeyLength + 1];
    BIO_read(pub, rsaPubKey, publicKeyLength);
    rsaPubKey[publicKeyLength] = '\0';

    DecryptionKeys decryptionKeys = {DecryptionKeys::ToBytes(std::string(rsaPubKey)), aesKey};
    return decryptionKeys;
}

std::vector<StorageProposal> StorageController::SortProposals(const StorageOrder &order)
{
    boost::lock_guard<boost::mutex> lock(mutex);
    std::vector<StorageProposal> proposals = proposalsAgent.GetProposals(order.GetHash());
    if (!proposals.size()) {
        return {};
    }

    std::list<StorageProposal> sortedProposals = { *(proposals.begin()) };
    for (auto itProposal = ++(proposals.begin()); itProposal != proposals.end(); ++itProposal) {
        for (auto it = sortedProposals.begin(); it != sortedProposals.end(); ++it) {
            if (itProposal->rate < it->rate) { // TODO: add check last save file time, free space, etc. (SS)
                sortedProposals.insert(it, *itProposal);
                break;
            }
        }
    }

    std::vector<StorageProposal> bestProposals(sortedProposals.begin(), sortedProposals.end());

    return bestProposals;
}

bool StorageController::StartHandshake(const StorageProposal &proposal, const DecryptionKeys &keys)
{
    StorageHandshake handshake { std::time(nullptr), proposal.orderHash, proposal.GetHash(), DEFAULT_DFS_PORT, keys };
    CNode* pNode = FindNode(proposal.address);
    if (pNode == nullptr) {
        for (int64_t nLoop = 0; nLoop < 100 && pNode == nullptr; nLoop++) {
            CAddress addr;
            OpenNetworkConnection(addr, false, NULL, proposal.address.ToStringIPPort().c_str());
            for (int i = 0; i < 10 && i < nLoop; i++) {
                MilliSleep(500);
            }
            pNode = FindNode(proposal.address);
            MilliSleep(500);
        }
    }

    if (pNode != nullptr) {
        pNode->PushMessage("dfshandshake", handshake);
        return true;
    }
    return false;
}

bool StorageController::FindReplicaKeepers(const StorageOrder &order, const int countReplica)
{
    std::vector<StorageProposal> proposals = SortProposals(order);
    int numReplica = 0;
    for (StorageProposal proposal : proposals) {
        if (AcceptProposal(proposal)) {
            if (++numReplica == countReplica) {
                boost::lock_guard<boost::mutex> lock(mutex);
                proposalsAgent.StopListenProposal(order.GetHash());
                return true;
            }
        }
    }
    return false;
}

std::shared_ptr<AllocatedFile> StorageController::CreateReplica(const boost::filesystem::path &sourcePath,
                                                                const StorageOrder &order,
                                                                const DecryptionKeys &keys,
                                                                RSA *rsa)
{
    namespace fs = boost::filesystem;

    std::ifstream filein;
    filein.open(sourcePath.string().c_str(), std::ios::binary);
    if (!filein.is_open()) {
        LogPrint("dfs", "file %s cannot be opened", sourcePath.string());
        return {};
    }

    auto length = fs::file_size(sourcePath);

    std::shared_ptr<AllocatedFile> tempFile = tempStorageHeap.AllocateFile(order.fileURI, GetCryptoReplicaSize(length));

    std::ofstream outfile;
    outfile.open(tempFile->fullpath.string(), std::ios::binary);

    uint64_t sizeBuffer = nBlockSizeRSA - 2;
    byte *buffer = new byte[sizeBuffer];
    byte *replica = new byte[nBlockSizeRSA];
    for (auto i = 0u; i < length; i+= sizeBuffer)
    {
        uint64_t n = std::min(sizeBuffer, order.fileSize - i);

        filein.read((char *)buffer, n); // TODO: change to loop of readsome
//        if (n < sizeBuffer) {
//            std::cout << "tail " << n << std::endl;
//        }
        EncryptData(buffer, 0, n, replica, keys.aesKey, rsa);
        outfile.write((char *) replica, nBlockSizeRSA);
        // TODO: check write (SS)
    }
    tempStorageHeap.SetDecryptionKeys(tempFile->uri, keys.rsaKey, keys.aesKey);
    filein.close();
    outfile.close();
    delete[] buffer;
    delete[] replica;

    return tempFile;
}

bool StorageController::SendReplica(const StorageOrder &order, const uint256 merkleRootHash, std::shared_ptr<AllocatedFile> pAllocatedFile, CNode* pNode)
{
    namespace fs = boost::filesystem;
    if (!pNode) {
        LogPrint("dfs", "Node does not found");
        return false;
    }
    ReplicaStream replicaStream;
    replicaStream.currentOrderHash = order.GetHash();
    replicaStream.merkleRootHash = merkleRootHash;
    replicaStream.filestream.open(pAllocatedFile->fullpath.string(), std::ios::binary|std::ios::in);
    if (!replicaStream.filestream.is_open()) {
        LogPrint("dfs", "file %s cannot be opened", pAllocatedFile->fullpath.string());
        return false;
    }
    pNode->PushMessage("dfssendfile", replicaStream);

    replicaStream.filestream.close();

    tempStorageHeap.FreeFile(pAllocatedFile->uri);
    fs::remove(pAllocatedFile->fullpath);
    return true;
}

bool StorageController::CheckReceivedReplica(const uint256 &orderHash, const uint256 &receivedMerkleRootHash, const boost::filesystem::path &replica)
{
    namespace fs = boost::filesystem;

    auto *order = GetAnnounce(orderHash);
    if (order) {
        size_t size = fs::file_size(replica);
        if (size != GetCryptoReplicaSize(order->fileSize)) {
            LogPrint("dfs", "Wrong file \"%s\" size. real size: %d not equal order size: %d ", order->filename, size,  GetCryptoReplicaSize(order->fileSize));
            return false;
        }
        { // check merkle root hash
            auto pMerleTreeFile = tempStorageHeap.AllocateFile(uint256{}, size);
            uint256 merkleRootHash = Merkler::ConstructMerkleTree(replica, pMerleTreeFile->fullpath);
            fs::remove(pMerleTreeFile->fullpath);
            tempStorageHeap.FreeFile(pMerleTreeFile->uri);
            if (merkleRootHash.ToString() != receivedMerkleRootHash.ToString()) {
                LogPrint("dfs", "Wrong merkle root hash. real hash: \"%s\" != \"%s\"(received)", merkleRootHash.ToString(), receivedMerkleRootHash.ToString());
                return false;
            }
        }
        return true;
    }
    return false;
}

void StorageController::BackgroundJob()
{
    auto lastCheckIp = std::time(nullptr);

    while (1) {
        boost::this_thread::interruption_point();
        boost::this_thread::sleep(boost::posix_time::seconds(1));

        if (!address.IsValid() || (std::time(nullptr) - lastCheckIp > 3600))
        {
            std::vector<CNode*> vNodesCopy;
            {
                LOCK(cs_vNodes);
                vNodesCopy = vNodes;
            }
            for (const auto &node : vNodesCopy) {
                node->PushMessage("dfsping");
            }
            if (std::time(nullptr) - lastCheckIp > 3600) {
                lastCheckIp = std::time(nullptr);
            }
        }

        std::vector<uint256> orderHashes;
        {
            boost::lock_guard<boost::mutex> lock(mutex);
            orderHashes = proposalsAgent.GetListenProposals();
        }
        for(auto &&orderHash : orderHashes) {
            const auto *order = GetAnnounce(orderHash);
            if (order != nullptr) {
                if(std::time(nullptr) > order->time + 60) {
                    FindReplicaKeepers(*order, 1);
                    boost::lock_guard<boost::mutex> lock(mutex);
                    proposalsAgent.StopListenProposal(orderHash);
                }
            }
        }
    }
}
