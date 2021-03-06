// Sandstorm - Personal Cloud Sandbox
// Copyright (c) 2014, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// This file is part of the Sandstorm platform implementation.
//
// Sandstorm is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// Sandstorm is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public
// License along with Sandstorm.  If not, see
// <http://www.gnu.org/licenses/>.

// This is a tool for manipulating Sandstorm .spk files.

#include <kj/main.h>
#include <kj/debug.h>
#include <kj/io.h>
#include <capnp/serialize.h>
#include <sodium.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <errno.h>
#include <sandstorm/package.capnp.h>
#include <stdlib.h>
#include <dirent.h>
#include <set>

namespace sandstorm {

typedef kj::byte byte;

// =======================================================================================
// base32 encode/decode derived from google-authenticator code, Apache 2.0 license:
//   https://code.google.com/p/google-authenticator/source/browse/libpam/base32.c
//
// Modifications:
// - Prefer to output in lower-case letters.
// - Use Douglas Crockford's alphabet mapping, except instead of excluding 'u', consider 'B' to
//   be a misspelling of '8'.
// - Use a lookup table for decoding (in addition to encoding).  Generate this table
//   programmatically at compile time.  C++14 constexpr is awesome.
// - Convert to KJ style.
//
// TODO(test):  This could use a unit test.

constexpr char BASE32_ENCODE_TABLE[] = "0123456789acdefghjkmnpqrstuvwxyz";

kj::String base32Encode(kj::ArrayPtr<const byte> data) {
  // We'll need a character for every 5 bits, rounded up.
  auto result = kj::heapString((data.size() * 8 + 4) / 5);

  uint count = 0;
  if (data.size() > 0) {
    uint buffer = data[0];
    uint next = 1;
    uint bitsLeft = 8;
    while (bitsLeft > 0 || next < data.size()) {
      if (bitsLeft < 5) {
        if (next < data.size()) {
          buffer <<= 8;
          buffer |= data[next++] & 0xFF;
          bitsLeft += 8;
        } else {
          // No more input; pad with zeros.
          uint pad = 5 - bitsLeft;
          buffer <<= pad;
          bitsLeft += pad;
        }
      }
      uint index = 0x1F & (buffer >> (bitsLeft - 5));
      bitsLeft -= 5;
      KJ_ASSERT(count < result.size());
      result[count++] = BASE32_ENCODE_TABLE[index];
    }
  }

  return result;
}

class Base32Decoder {
public:
  constexpr Base32Decoder(): decodeTable() {
    // Cool, we can generate our lookup table at compile time.

    for (byte& b: decodeTable) {
      b = 255;
    }

    for (uint i = 0; i < sizeof(BASE32_ENCODE_TABLE) - 1; i++) {
      unsigned char c = BASE32_ENCODE_TABLE[i];
      decodeTable[c] = i;
      if ('a' <= c && c <= 'z') {
        decodeTable[c - 'a' + 'A'] = i;
      }
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wchar-subscripts"
    decodeTable['o'] = decodeTable['O'] = 0;
    decodeTable['i'] = decodeTable['I'] = 1;
    decodeTable['l'] = decodeTable['L'] = 1;
    decodeTable['b'] = decodeTable['B'] = 8;
#pragma GCC diagnostic pop
  }

  constexpr bool verifyTable() const {
    // Verify that all letters and digits have a decoding.
    //
    // Oh cool, this can also be done at compile time, and then checked with a static_assert below.
    //
    // C++14 is awesome.

    for (unsigned char c = '0'; c <= '9'; c++) {
      if (decodeTable[c] == 255) return false;
    }
    for (unsigned char c = 'a'; c <= 'z'; c++) {
      if (decodeTable[c] == 255) return false;
    }
    for (unsigned char c = 'A'; c <= 'Z'; c++) {
      if (decodeTable[c] == 255) return false;
    }
    return true;
  }

  kj::Array<byte> decode(kj::StringPtr encoded) const {
    // We intentionally round the size down.  Leftover bits are presumably zero.
    auto result = kj::heapArray<byte>(encoded.size() * 5 / 8);

    uint buffer = 0;
    uint bitsLeft = 0;
    uint count = 0;
    for (char c: encoded) {
      byte decoded = decodeTable[(byte)c];
      KJ_ASSERT(decoded <= 32, "Invalid base32.");

      buffer <<= 5;
      buffer |= decoded;
      bitsLeft += 5;
      if (bitsLeft >= 8) {
        KJ_ASSERT(count < encoded.size());
        bitsLeft -= 8;
        result[count++] = buffer >> bitsLeft;
      }
    }

    buffer &= (1 << bitsLeft) - 1;
    KJ_REQUIRE(buffer == 0, "Base32 decode failed: extra bits at end.");

    return result;
  }

private:
  byte decodeTable[256];
};

constexpr Base32Decoder BASE64_DECODER;
static_assert(BASE64_DECODER.verifyTable(), "Base32 decode table is incomplete.");

// =======================================================================================

kj::AutoCloseFd raiiOpen(kj::StringPtr name, int flags, mode_t mode = 0666) {
  int fd;
  KJ_SYSCALL(fd = open(name.cStr(), flags, mode));
  return kj::AutoCloseFd(fd);
}

kj::AutoCloseFd openTemporary(kj::StringPtr near) {
  // Creates a temporary file in the same directory as the file specified by "near", immediately
  // unlinks it, and then returns the file descriptor,  which will be open for both read and write.

  // TODO(someday):  Use O_TMPFILE?  New in Linux 3.11.

  int fd;
  auto name = kj::str(near, ".XXXXXX");
  KJ_SYSCALL(fd = mkstemp(name.begin()));
  kj::AutoCloseFd result(fd);
  KJ_SYSCALL(unlink(name.cStr()));
  return result;
}

size_t getFileSize(int fd, kj::StringPtr filename) {
  struct stat stats;
  KJ_SYSCALL(fstat(fd, &stats));
  KJ_REQUIRE(S_ISREG(stats.st_mode), "Not a regular file.", filename);
  return stats.st_size;
}

class MemoryMapping {
public:
  MemoryMapping(): content(nullptr) {}

  explicit MemoryMapping(int fd, kj::StringPtr filename): content(nullptr) {
    size_t size = getFileSize(fd, filename);

    if (size != 0) {
      void* ptr = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
      if (ptr == MAP_FAILED) {
        KJ_FAIL_SYSCALL("mmap", errno, filename);
      }

      content = kj::arrayPtr(reinterpret_cast<byte*>(ptr), size);
    }
  }

  ~MemoryMapping() {
    if (content != nullptr) {
      KJ_SYSCALL(munmap(content.begin(), content.size()));
    }
  }

  KJ_DISALLOW_COPY(MemoryMapping);
  inline MemoryMapping(MemoryMapping&& other): content(other.content) {
    other.content = nullptr;
  }
  inline MemoryMapping& operator=(MemoryMapping&& other) {
    MemoryMapping old(kj::mv(*this));
    content = other.content;
    other.content = nullptr;
    return *this;
  }

  inline operator kj::ArrayPtr<const byte>() const {
    return content;
  }

  inline operator capnp::Data::Reader() const {
    return content;
  }

  inline operator kj::ArrayPtr<const capnp::word>() const {
    return kj::arrayPtr(reinterpret_cast<const capnp::word*>(content.begin()),
                        content.size() / sizeof(capnp::word));
  }

private:
  kj::ArrayPtr<byte> content;
};

class ChildProcess {
public:
  enum Direction {
    OUTPUT,
    INPUT
  };

  ChildProcess(kj::StringPtr command, kj::StringPtr flags,
               kj::AutoCloseFd wrappedFd, Direction direction) {
    int pipeFds[2];
    KJ_SYSCALL(pipe(pipeFds));
    kj::AutoCloseFd pipeInput(pipeFds[0]), pipeOutput(pipeFds[1]);

    KJ_SYSCALL(pid = fork());
    if (pid == 0) {
      if (direction == OUTPUT) {
        KJ_SYSCALL(dup2(pipeInput, STDIN_FILENO));
        KJ_SYSCALL(dup2(wrappedFd, STDOUT_FILENO));
      } else {
        KJ_SYSCALL(dup2(wrappedFd, STDIN_FILENO));
        KJ_SYSCALL(dup2(pipeOutput, STDOUT_FILENO));
      }
      pipeInput = nullptr;
      pipeOutput = nullptr;
      wrappedFd = nullptr;

      KJ_SYSCALL(execlp(command.cStr(), command.cStr(), flags.cStr(), (const char*)nullptr),
                 command);
      KJ_UNREACHABLE;
    } else {
      if (direction == OUTPUT) {
        pipeFd = kj::mv(pipeOutput);
      } else {
        pipeFd = kj::mv(pipeInput);
      }
    }
  }

  ~ChildProcess() {
    if (pid == 0) return;

    // Close the pipe first, in case the child is waiting for that.
    pipeFd = nullptr;

    int status;
    KJ_SYSCALL(waitpid(pid, &status, 0)) { return; }
    if (status != 0) {
      if (WIFEXITED(status)) {
        int exitCode = WEXITSTATUS(status);
        KJ_FAIL_ASSERT("child process failed", exitCode) { return; }
      } else if (WIFSIGNALED(status)) {
        int signalNumber = WTERMSIG(status);
        KJ_FAIL_ASSERT("child process crashed", signalNumber) { return; }
      } else {
        KJ_FAIL_ASSERT("child process failed") { return; }
      }
    }
  }

  int getPipe() { return pipeFd; }

  KJ_DISALLOW_COPY(ChildProcess);

private:
  kj::AutoCloseFd pipeFd;
  pid_t pid;
};

class SpkTool {
  // Main class for the Sandstorm spk tool.

public:
  SpkTool(kj::ProcessContext& context): context(context) {}

  kj::MainFunc getMain() {
    return kj::MainBuilder(context, "Sandstorm version 0.0",
          "Tool for building and checking Sandstorm package files.",
          "Sandstorm packages are tar.xz archives prefixed with a header containing a "
          "cryptographic signature in order to prove that upgrades came from the same source.  "
          "This tool will help you create and sign packages.")
        .addSubCommand("keygen", KJ_BIND_METHOD(*this, getKeygenMain),
                       "Generate a new keyfile.")
        .addSubCommand("appid", KJ_BIND_METHOD(*this, getAppidMain),
                       "Get the app ID corresponding to an existing keyfile.")
        .addSubCommand("pack", KJ_BIND_METHOD(*this, getPackMain),
                       "Create an spk from a directory tree and a signing key.")
        .addSubCommand("unpack", KJ_BIND_METHOD(*this, getUnpackMain),
                       "Unpack an spk to a directory, verifying its signature.")
        .build();
  }

private:
  kj::ProcessContext& context;
  bool onlyPrintId = false;

  bool setOnlyPrintId() { onlyPrintId = true; return true; }

  void printAppId(kj::ArrayPtr<const byte> publicKey, kj::StringPtr filename) {
    static_assert(crypto_sign_PUBLICKEYBYTES == 32, "Signing algorithm changed?");
    KJ_REQUIRE(publicKey.size() == crypto_sign_PUBLICKEYBYTES);

    auto appId = base32Encode(publicKey);
    kj::String msg = onlyPrintId ? kj::str(appId, "\n") : kj::str(appId, " ", filename, "\n");
    kj::FdOutputStream out(STDOUT_FILENO);
    out.write(msg.begin(), msg.size());
  }

  // =====================================================================================

  kj::MainFunc getKeygenMain() {
    return kj::MainBuilder(context, "Sandstorm version 0.0",
            "Create a new key pair and store it in <output>.  It can then be used as input to "
            "the `sign` command.  Make sure to store the output in a safe place!  If you lose it, "
            "you won't be able to update your app, and if someone else gets ahold of it, they'll "
            "be able to hijack your app.")
        .addOption({'o', "only-id"}, KJ_BIND_METHOD(*this, setOnlyPrintId),
            "Only print the app ID, not the file name.")
        .expectOneOrMoreArgs("<output>", KJ_BIND_METHOD(*this, genKeyFile))
        .build();
  }

  kj::MainBuilder::Validity genKeyFile(kj::StringPtr arg) {
    capnp::MallocMessageBuilder message;
    spk::KeyFile::Builder builder = message.getRoot<spk::KeyFile>();

    int result = crypto_sign_keypair(
        builder.initPublicKey(crypto_sign_PUBLICKEYBYTES).begin(),
        builder.initPrivateKey(crypto_sign_SECRETKEYBYTES).begin());
    KJ_ASSERT(result == 0, "crypto_sign_keypair failed", result);

    int fd;
    KJ_SYSCALL(fd = open(arg.cStr(), O_WRONLY | O_CREAT | O_TRUNC, 0666));
    kj::AutoCloseFd closer(fd);
    capnp::writeMessageToFd(fd, message);

    // Notify the caller of the app ID.
    printAppId(builder.getPublicKey(), arg);

    return true;
  }

  // =====================================================================================

  kj::MainFunc getAppidMain() {
    return kj::MainBuilder(context, "Sandstorm version 0.0",
            "Read <keyfile> and extract the textual app ID, printing it to stdout.")
        .addOption({'o', "only-id"}, KJ_BIND_METHOD(*this, setOnlyPrintId),
            "Only print the app ID, not the file name.")
        .expectOneOrMoreArgs("<keyfile>", KJ_BIND_METHOD(*this, getAppIdFromKeyfile))
        .build();
  }

  kj::MainBuilder::Validity getAppIdFromKeyfile(kj::StringPtr arg) {
    if (access(arg.cStr(), F_OK) != 0) {
      return "No such file.";
    }

    // Read the keyfile.
    MemoryMapping keyfile(raiiOpen(arg, O_RDONLY), arg);
    capnp::FlatArrayMessageReader keyMessage(keyfile);
    spk::KeyFile::Reader keyReader = keyMessage.getRoot<spk::KeyFile>();
    KJ_REQUIRE(keyReader.getPublicKey().size() == crypto_sign_PUBLICKEYBYTES &&
               keyReader.getPrivateKey().size() == crypto_sign_SECRETKEYBYTES,
               "Invalid key file.");

    printAppId(keyReader.getPublicKey(), arg);
    return true;
  }

  // =====================================================================================

  kj::String dirname;
  kj::String keyfile;
  kj::String spkfile;
  kj::Vector<MemoryMapping> mappings;

  kj::MainFunc getPackMain() {
    return kj::MainBuilder(context, "Sandstorm version 0.0",
            "Pack the contents of <dirname> as an spk, signing it using <keyfile>, and writing "
            "the result to <output>.  If <output> is not specified, it will be formed by "
            "appending \".spk\" to the directory name.")
        .addOption({'o', "only-id"}, KJ_BIND_METHOD(*this, setOnlyPrintId),
            "Only print the app ID, not the file name.")
        .expectArg("<dirname>", KJ_BIND_METHOD(*this, setDirname))
        .expectArg("<keyfile>", KJ_BIND_METHOD(*this, setKeyfile))
        .expectOptionalArg("<output>", KJ_BIND_METHOD(*this, setSpkfile))
        .callAfterParsing(KJ_BIND_METHOD(*this, doPack))
        .build();
  }

  kj::MainBuilder::Validity setDirname(kj::StringPtr name) {
    if (access(name.cStr(), F_OK) < 0) {
      return "Not found.";
    }

    dirname = kj::heapString(name);
    spkfile = kj::str(name, ".spk");
    return true;
  }

  kj::MainBuilder::Validity setKeyfile(kj::StringPtr name) {
    if (access(name.cStr(), F_OK) < 0) {
      return "No such file.";
    }

    keyfile = kj::heapString(name);
    return true;
  }

  kj::MainBuilder::Validity setSpkfile(kj::StringPtr name) {
    spkfile = kj::heapString(name);
    return true;
  }

  void packFile(spk::Archive::File::Builder file, kj::StringPtr dirname, kj::StringPtr filename) {
    // Construct an Archive.File from a disk file.

    file.setName(filename);

    auto path = kj::str(dirname, '/', filename);

    struct stat stats;
    KJ_SYSCALL(lstat(path.cStr(), &stats), path);

    auto orphanage = capnp::Orphanage::getForMessageContaining(file);

    if (S_ISREG(stats.st_mode)) {
      MemoryMapping mapping(raiiOpen(path, O_RDONLY), path);
      auto content = orphanage.referenceExternalData(mapping);
      mappings.add(kj::mv(mapping));

      if (stats.st_mode & S_IXUSR) {
        file.adoptExecutable(kj::mv(content));
      } else {
        file.adoptRegular(kj::mv(content));
      }
    } else if (S_ISLNK(stats.st_mode)) {
      auto symlink = file.initSymlink(stats.st_size);

      ssize_t linkSize;
      KJ_SYSCALL(linkSize = readlink(path.cStr(), symlink.begin(), stats.st_size), path);
      KJ_ASSERT(linkSize == stats.st_size, "Link changed between stat() and readlink().", path);
    } else if (S_ISDIR(stats.st_mode)) {
      file.adoptDirectory(packDirectory(orphanage, path));
    } else {
      context.warning(kj::str("Cannot pack irregular file: ", path));
    }
  }

  capnp::Orphan<capnp::List<spk::Archive::File>> packDirectory(
      capnp::Orphanage orphanage, kj::StringPtr dirname) {
    // Construct a list of Archive.Files from a disk directory.

    DIR* dir = opendir(dirname.cStr());
    if (dir == nullptr) {
      KJ_FAIL_SYSCALL("opendir", errno, dirname);
    }
    KJ_DEFER(closedir(dir));

    kj::Vector<kj::String> entries;

    for (;;) {
      errno = 0;
      struct dirent* entry = readdir(dir);
      if (entry == nullptr) {
        int error = errno;
        if (error == 0) {
          break;
        } else {
          KJ_FAIL_SYSCALL("readdir", error, dirname);
        }
      }

      kj::StringPtr name = entry->d_name;
      if (name != "." && name != "..") {
        entries.add(kj::heapString(entry->d_name));
      }
    }

    auto result = orphanage.newOrphan<capnp::List<spk::Archive::File>>(entries.size());
    auto list = result.get();

    for (uint i: kj::indices(entries)) {
      packFile(list[i], dirname, entries[i]);
    }

    return result;
  }

  kj::MainBuilder::Validity doPack() {
    // Read the keyfile.
    MemoryMapping keyfile(raiiOpen(this->keyfile, O_RDONLY), this->keyfile);
    capnp::FlatArrayMessageReader keyMessage(keyfile);
    spk::KeyFile::Reader keyReader = keyMessage.getRoot<spk::KeyFile>();
    KJ_REQUIRE(keyReader.getPublicKey().size() == crypto_sign_PUBLICKEYBYTES &&
               keyReader.getPrivateKey().size() == crypto_sign_SECRETKEYBYTES,
               "Invalid key file.");

    auto tmpfile = openTemporary(this->spkfile);

    {
      // Write the archive.
      capnp::MallocMessageBuilder archiveMessage;
      auto archive = archiveMessage.getRoot<spk::Archive>();
      archive.adoptFiles(packDirectory(archiveMessage.getOrphanage(), dirname));
      capnp::writeMessageToFd(tmpfile, archiveMessage);

      // We can unmap all the mappings now that we've copied them.
      mappings.resize(0);
    }

    // Map the temp file back in.
    MemoryMapping tmpMapping(tmpfile, this->spkfile);
    kj::ArrayPtr<const byte> tmpData = tmpMapping;

    // Hash it.
    byte hash[crypto_hash_BYTES];
    crypto_hash(hash, tmpData.begin(), tmpData.size());

    // Generate the signature.
    capnp::MallocMessageBuilder signatureMessage;
    spk::Signature::Builder signature = signatureMessage.getRoot<spk::Signature>();
    signature.setPublicKey(keyReader.getPublicKey());
    unsigned long long siglen = crypto_hash_BYTES + crypto_sign_BYTES;
    crypto_sign(signature.initSignature(siglen).begin(), &siglen,
                hash, sizeof(hash), keyReader.getPrivateKey().begin());

    // Now write the whole thing out.
    {
      auto finalFile = raiiOpen(spkfile, O_WRONLY | O_CREAT | O_TRUNC);

      // Write magic number uncompressed.
      auto magic = spk::MAGIC_NUMBER.get();
      kj::FdOutputStream(finalFile.get()).write(magic.begin(), magic.size());

      // Pipe content through xz compressor.
      ChildProcess child("xz", "-zc", kj::mv(finalFile), ChildProcess::OUTPUT);

      // Write signature and archive out to the pipe.
      kj::FdOutputStream out(child.getPipe());
      capnp::writeMessage(out, signatureMessage);
      out.write(tmpData.begin(), tmpData.size());
    }

    printAppId(keyReader.getPublicKey(), this->spkfile);

    return true;
  }

  // =====================================================================================

  kj::MainFunc getUnpackMain() {
    return kj::MainBuilder(context, "Sandstorm version 0.0",
            "Check that <spkfile>'s signature is valid.  If so, unpack it to <outdir> and "
            "print the app ID and filename.  If <outdir> is not specified, it will be "
            "chosen by removing the suffix \".spk\" from the input file name.")
        .addOption({'o', "only-id"}, KJ_BIND_METHOD(*this, setOnlyPrintId),
            "Only print the app ID, not the file name.")
        .expectArg("<spkfile>", KJ_BIND_METHOD(*this, setUnpackSpkfile))
        .expectOptionalArg("<outdir>", KJ_BIND_METHOD(*this, setUnpackDirname))
        .callAfterParsing(KJ_BIND_METHOD(*this, doUnpack))
        .build();
  }

  kj::MainBuilder::Validity setUnpackSpkfile(kj::StringPtr name) {
    if (access(name.cStr(), F_OK) < 0) {
      return "Not found.";
    }

    spkfile = kj::heapString(name);
    if (spkfile.endsWith(".spk")) {
      dirname = kj::heapString(spkfile.slice(0, spkfile.size() - 4));
    }

    return true;
  }

  kj::MainBuilder::Validity setUnpackDirname(kj::StringPtr name) {
    if (access(name.cStr(), F_OK) == 0) {
      return "Already exists.";
    }

    dirname = kj::heapString(name);
    return true;
  }

  kj::MainBuilder::Validity validationError(kj::StringPtr filename, kj::StringPtr problem) {
    context.exitError(kj::str("*** ", filename, ": ", problem));
  }

  kj::MainBuilder::Validity doUnpack() {
    if (access(dirname.cStr(), F_OK) == 0) {
      return "Output directory already exists.";
    }

    byte publicKey[crypto_sign_PUBLICKEYBYTES];
    byte sigBytes[crypto_hash_BYTES + crypto_sign_BYTES];
    byte expectedHash[sizeof(sigBytes)];
    unsigned long long hashLength = 0;  // will be overwritten later

    auto tmpfile = openTemporary(spkfile);

    // Read the spk, checking the magic number, reading the signature header, and decompressing the
    // archive to a temp file.
    {
      // Open the spk.
      auto spkfd = raiiOpen(spkfile, O_RDONLY);

      // TODO(security):  We could at this point chroot into the output directory and unshare
      //   various resources for extra security, if not for the fact that we need to invoke xz
      //   later on.  Maybe link against the xz library so that we don't have to exec it?

      // Check the magic number.
      auto expectedMagic = spk::MAGIC_NUMBER.get();
      byte magic[expectedMagic.size()];
      kj::FdInputStream(spkfd.get()).read(magic, expectedMagic.size());
      for (uint i: kj::indices(expectedMagic)) {
        if (magic[i] != expectedMagic[i]) {
          return validationError(spkfile, "Does not appear to be an .spk (bad magic number).");
        }
      }

      // Decompress the remaining bytes in the SPK using xz.
      auto child = kj::heap<ChildProcess>("xz", "-dc", kj::mv(spkfd), ChildProcess::INPUT);
      kj::FdInputStream in(child->getPipe());

      // Read in the signature.
      {
        capnp::InputStreamMessageReader signatureMessage(in);
        auto signature = signatureMessage.getRoot<spk::Signature>();
        auto pkReader = signature.getPublicKey();
        if (pkReader.size() != sizeof(publicKey)) {
          return validationError(spkfile, "Invalid public key.");
        }
        memcpy(publicKey, pkReader.begin(), sizeof(publicKey));
        auto sigReader = signature.getSignature();
        if (sigReader.size() != sizeof(sigBytes)) {
          return validationError(spkfile, "Invalid signature format.");
        }
        memcpy(sigBytes, sigReader.begin(), sizeof(sigBytes));
      }

      // Verify the signature.
      int result = crypto_sign_open(
          expectedHash, &hashLength, sigBytes, sizeof(sigBytes), publicKey);
      if (result != 0) {
        return validationError(spkfile, "Invalid signature.");
      }
      if (hashLength != crypto_hash_BYTES) {
        return validationError(spkfile, "Wrong signature size.");
      }

      // Copy archive part to a temp file.
      kj::FdOutputStream tmpOut(tmpfile.get());
      for (;;) {
        byte buffer[8192];
        size_t n = in.tryRead(buffer, 1, sizeof(buffer));
        if (n == 0) break;
        tmpOut.write(buffer, n);
      }
    }

    // mmap the temp file.
    MemoryMapping tmpMapping(tmpfile, "(temp file)");
    tmpfile = nullptr;  // We have the mapping now; don't need the fd.

    // Hash the archive.
    kj::ArrayPtr<const byte> tmpBytes = tmpMapping;
    byte hash[crypto_hash_BYTES];
    crypto_hash(hash, tmpBytes.begin(), tmpBytes.size());

    // Check that hashes match.
    if (memcmp(expectedHash, hash, crypto_hash_BYTES) != 0) {
      return validationError(spkfile, "Signature didn't match package contents.");
    }

    // Set up archive reader.
    kj::ArrayPtr<const capnp::word> tmpWords = tmpMapping;
    capnp::ReaderOptions options;
    options.traversalLimitInWords = tmpWords.size();
    capnp::FlatArrayMessageReader archiveMessage(tmpWords, options);

    // Unpack.
    KJ_SYSCALL(mkdir(dirname.cStr(), 0777), dirname);
    unpackDir(archiveMessage.getRoot<spk::Archive>().getFiles(), dirname);

    // Note the appid.
    printAppId(publicKey, spkfile);

    return true;
  }

  void unpackDir(capnp::List<spk::Archive::File>::Reader files, kj::StringPtr dirname) {
    std::set<kj::StringPtr> seen;

    for (auto file: files) {
      kj::StringPtr name = file.getName();
      KJ_REQUIRE(name.size() != 0 && name != "." && name != ".." &&
                 name.findFirst('/') == nullptr && name.findFirst('\0') == nullptr,
                 "Archive contained invalid file name.", name);

      KJ_REQUIRE(seen.insert(name).second, "Archive contained duplicate file name.", name);

      auto path = kj::str(dirname, '/', name);

      KJ_ASSERT(access(path.cStr(), F_OK) != 0, "Unpacked file already exists.", path);

      switch (file.which()) {
        case spk::Archive::File::REGULAR: {
          auto bytes = file.getRegular();
          kj::FdOutputStream(raiiOpen(path, O_WRONLY | O_CREAT | O_EXCL, 0666))
              .write(bytes.begin(), bytes.size());
          break;
        }

        case spk::Archive::File::EXECUTABLE: {
          auto bytes = file.getExecutable();
          kj::FdOutputStream(raiiOpen(path, O_WRONLY | O_CREAT | O_EXCL, 0777))
              .write(bytes.begin(), bytes.size());
          break;
        }

        case spk::Archive::File::SYMLINK: {
          KJ_SYSCALL(symlink(file.getSymlink().cStr(), path.cStr()), path);
          break;
        }

        case spk::Archive::File::DIRECTORY: {
          KJ_SYSCALL(mkdir(path.cStr(), 0777), path);
          unpackDir(file.getDirectory(), path);
          break;
        }

        default:
          KJ_FAIL_REQUIRE("Unknown file type in archive.");
      }
    }
  }
};

}  // namespace sandstorm

KJ_MAIN(sandstorm::SpkTool)
