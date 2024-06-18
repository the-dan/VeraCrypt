/*
 Derived from source code of TrueCrypt 7.1a, which is
 Copyright (c) 2008-2012 TrueCrypt Developers Association and which is governed
 by the TrueCrypt License 3.0.

 Modifications and additions to the original source code (contained in this file)
 and all other portions of this file are Copyright (c) 2013-2017 IDRIX
 and are governed by the Apache License 2.0 the full text of which is
 contained in the file License.txt included in VeraCrypt binary and source
 code distribution packages.
*/

#include "Platform/Serializer.h"
#include "Common/SecurityToken.h"
#include "Platform/MemoryStream.h"
#include "Platform/PipelineStream.h"
#include "Platform/FileStream.h"
#include "Crc32.h"
#include "Keyfile.h"
#include "VolumeException.h"

namespace VeraCrypt
{
	void Keyfile::Apply (const BufferPtr &pool, wstring tokenKeyDescriptor) const {
		if (Path.IsDirectory())
			throw ParameterIncorrect (SRC_POS);

		Crc32 crc32;
		size_t poolPos = 0;
		uint64 totalLength = 0;
		uint64 readLength;

		shared_ptr<Stream> s = PrepareStream(tokenKeyDescriptor);

		SecureBuffer keyfileBuf (File::GetOptimalReadSize());
		File encryptedKeyfile;

		
		while ((readLength = s->Read (keyfileBuf)) > 0)
		{
			for (int i = 0; i < readLength; i++) {
				uint32 crc = crc32.Process (keyfileBuf[i]);

				pool[poolPos++] += (byte) (crc >> 24);
				pool[poolPos++] += (byte) (crc >> 16);
				pool[poolPos++] += (byte) (crc >> 8);
				pool[poolPos++] += (byte) crc;

				if (poolPos >= pool.Size())
					poolPos = 0;

				if (++totalLength >= MaxProcessedLength)
					break;
			}
		}
	}


	shared_ptr <VolumePassword> Keyfile::ApplyListToPassword (shared_ptr <KeyfileList> keyfiles, shared_ptr <VolumePassword> password,
		wstring tokenDescriptor)
	{
		if (!password)
			password.reset (new VolumePassword);

		if (!keyfiles || keyfiles->size() < 1)
			return password;

		KeyfileList keyfilesExp;
		HiddenFileWasPresentInKeyfilePath = false;

		// Enumerate directories
		foreach (shared_ptr <Keyfile> keyfile, *keyfiles)
		{
			if (FilesystemPath (*keyfile).IsDirectory())
			{
				size_t keyfileCount = 0;
				foreach_ref (const FilePath &path, Directory::GetFilePaths (*keyfile))
				{
#ifdef TC_UNIX
					// Skip hidden files
					if (wstring (path.ToBaseName()).find (L'.') == 0)
					{
						HiddenFileWasPresentInKeyfilePath = true;
						continue;
					}
#endif
					keyfilesExp.push_back (make_shared <Keyfile> (path));
					++keyfileCount;
				}

				if (keyfileCount == 0) {
					throw KeyfilePathEmpty (SRC_POS, FilesystemPath (*keyfile));
				}
			}
			else
			{
				keyfilesExp.push_back (keyfile);
			}
		}

		make_shared_auto (VolumePassword, newPassword);

		if (keyfilesExp.size() < 1)
		{
			newPassword->Set (*password);
		}
		else
		{
			SecureBuffer keyfilePool (password->Size() <= VolumePassword::MaxLegacySize? VolumePassword::MaxLegacySize: VolumePassword::MaxSize);

			// Pad password with zeros if shorter than max length
			keyfilePool.Zero();
			keyfilePool.CopyFrom (ConstBufferPtr (password->DataPtr(), password->Size()));

			// Apply all keyfiles
			foreach_ref (const Keyfile &k, keyfilesExp)
			{
				k.Apply (keyfilePool, tokenDescriptor);
			}

			newPassword->Set (keyfilePool);
		}

		return newPassword;
	}

	shared_ptr <KeyfileList> Keyfile::DeserializeList (shared_ptr <Stream> stream, const string &name)
	{
		shared_ptr <KeyfileList> keyfiles;
		Serializer sr (stream);

		if (!sr.DeserializeBool (name + "Null"))
		{
			keyfiles.reset (new KeyfileList);
			foreach (const wstring &k, sr.DeserializeWStringList (name))
				keyfiles->push_back (make_shared <Keyfile> (k));
		}
		return keyfiles;
	}

	void Keyfile::SerializeList (shared_ptr <Stream> stream, const string &name, shared_ptr <KeyfileList> keyfiles)
	{
		Serializer sr (stream);
		sr.Serialize (name + "Null", keyfiles == nullptr);
		if (keyfiles)
		{
			list <wstring> sl;

			foreach_ref (const Keyfile &k, *keyfiles)
				sl.push_back (FilesystemPath (k));

			sr.Serialize (name, sl);
		}
	}

	void Keyfile::CreateBluekey(FilePath bluekeyFile, wstring tokenKeyDescriptor, SecureBuffer &buffer) {
		SecurityTokenKey key;
		SecurityToken::GetSecurityTokenKey(tokenKeyDescriptor, key, SecurityTokenKeyOperation::ENCRYPT);

		size_t inputBufferSize = key.maxDecryptBufferSize;
		size_t outputBufferSize = key.maxEncryptBufferSize;

		vector<byte> tokenDataToProcess;
		vector<byte> processedData(outputBufferSize);

		tokenDataToProcess.reserve(inputBufferSize);

		BufferPtr remainder;
		if (buffer.Size() >= inputBufferSize) {
			std::copy(buffer.Ptr(), buffer.Ptr() + inputBufferSize, back_inserter(tokenDataToProcess));
			remainder = buffer.GetRange(inputBufferSize, buffer.Size() - inputBufferSize);
		} else {
			// buffer size less than encryption buffer size
			// in order to provide the best security, we shouldn't work with such keyfiles
			throw InsufficientData();
		}
		
		SecurityToken::GetEncryptedData(key, tokenDataToProcess, processedData);
		SecureBuffer result(ConstBufferPtr(processedData.data(), processedData.size()));

		PipelineStream bkfs;
		auto m = make_shared<MemoryStream>(result);

		bkfs.AddStream(m);
		
		if (remainder.Size() > 0) {
			auto r = make_shared<MemoryStream>(remainder);
			bkfs.AddStream(r);
		}

		File keyfile;
		keyfile.Open (bluekeyFile, File::CreateWrite);

		size_t n;
		SecureBuffer writeBuffer(File::GetOptimalWriteSize());
		while ((n = bkfs.Read(writeBuffer)) > 0) {
			keyfile.Write (writeBuffer, n);
		}
	}

	void Keyfile::RevealRedkey(FilePath redkey, wstring tokenKeyDescriptor) {
		shared_ptr<Stream> kfs = PrepareStream(tokenKeyDescriptor);	

		File redKey;
		redKey.Open (redkey, File::CreateWrite, File::ShareReadWriteIgnoreLock);
		size_t read;
		SecureBuffer buffer (File::GetOptimalReadSize());
		while ((read = kfs->Read(buffer)) > 0) {
			redKey.Write(buffer, read);
		}
		redKey.Close();
	}
	
	shared_ptr<Stream> Keyfile::PrepareStream(wstring tokenKeyDescriptor) const {
		if (SecurityToken::IsKeyfilePathValid (Path))
		{
			// Apply keyfile generated by a security token
			vector <byte> keyfileData;
			SecurityToken::GetKeyfileData (SecurityTokenKeyfile (wstring (Path)), keyfileData);

			if (keyfileData.size() < MinProcessedLength)
				throw InsufficientData (SRC_POS, Path);

			MemoryStream *ms = new MemoryStream(ConstBufferPtr(keyfileData.data(), keyfileData.size()));
			return shared_ptr<Stream>(ms);
		}

		shared_ptr<PipelineStream> ps = shared_ptr<PipelineStream>(new PipelineStream());
		
		shared_ptr<File> file = shared_ptr<File> (new File());
		SecureBuffer keyfileBuf (File::GetOptimalReadSize());
		uint64 readLength;

		File encryptedKeyfile;

		
		file->Open (Path, File::OpenRead, File::ShareRead);

		if (!tokenKeyDescriptor.empty()) {
			// if token is specified, first part of the file is/should be encrypted

			
			// get token slot and key from descriptor
			SecurityTokenKey key;
			SecurityToken::GetSecurityTokenKey(tokenKeyDescriptor, key, SecurityTokenKeyOperation::DECRYPT);
			
			// set proper vector size based on key length
			// and mode (encryption/decryption)
			size_t inputBufferSize = key.maxEncryptBufferSize;
			size_t outputBufferSize = key.maxDecryptBufferSize;
			uint64 appendBytesCount;

			vector<byte> tokenDataToProcess;
			vector<byte> processedData(outputBufferSize);


			while ((readLength = file->Read (keyfileBuf)) > 0)
			{
				if (tokenDataToProcess.size() < inputBufferSize) {
					appendBytesCount = readLength;
					if (tokenDataToProcess.size() + appendBytesCount > inputBufferSize) {
						appendBytesCount = inputBufferSize - tokenDataToProcess.size();
						tokenDataToProcess.insert(tokenDataToProcess.end(), keyfileBuf.Ptr(), keyfileBuf.Ptr() + appendBytesCount);
						break;
					}
					tokenDataToProcess.insert(tokenDataToProcess.end(), keyfileBuf.Ptr(), keyfileBuf.Ptr() + appendBytesCount);
				}
			}

			string s(tokenDataToProcess.begin(), tokenDataToProcess.end());

			
			SecurityToken::GetDecryptedData(key, tokenDataToProcess, processedData);
			
			auto tokenStream = make_shared<MemoryStream>(ConstBufferPtr(processedData.data(), processedData.size()));
			ps->AddStream(tokenStream);

			// process the rest of the buffer as an ordinary (non-encrypted) data
			// otherwise we'd get non-deterministic behavior, because Read() could produce buffers of different sizes
			BufferPtr remainderBuffer = keyfileBuf.GetRange(appendBytesCount, readLength-appendBytesCount);
			if (encryptedKeyfile.IsOpen()) {
				// dump read but unused chunk first
				if (appendBytesCount < readLength) {
					string ss(keyfileBuf.Ptr() + appendBytesCount, keyfileBuf.Ptr() + readLength);
					encryptedKeyfile.Write(ConstBufferPtr(&keyfileBuf[appendBytesCount], readLength-appendBytesCount));
				}
			}

			auto remainderStream = make_shared<MemoryStream>(remainderBuffer);
			ps->AddStream(remainderStream);
		}

		auto fs = make_shared<FileStream>(file);
		ps->AddStream(fs);
		
		return ps;

	}

	bool Keyfile::HiddenFileWasPresentInKeyfilePath = false;
}
