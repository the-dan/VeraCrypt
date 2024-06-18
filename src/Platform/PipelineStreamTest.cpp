#include "Testing.h"
#include "PipelineStream.h"
#include "MemoryStream.h"
#include "Stream.h"


using namespace VeraCrypt; 

#define MK(type, name) shared_ptr<type> name = shared_ptr<type>(new type());

size_t ReadFully(shared_ptr<Stream> s, Buffer *rb, int chunkSize = 10) {
    vector<Buffer*> buffers;
    vector<int> lengths;
    size_t n = 0;
    size_t tot = 0;
    Buffer *buff = new Buffer(chunkSize);
    while ((n = s->Read(*buff)) > 0) {
        buffers.push_back(buff);
        lengths.push_back(n);
        buff = new Buffer(chunkSize);
        tot += n;
    }

    Buffer *buffer = new Buffer(tot);
    size_t offset = 0;
    for (size_t i = 0; i < buffers.size(); i++) {
        std::memcpy(buffer->Ptr()+offset, buffers[i]->GetRange(0, lengths[i]), lengths[i]);
        offset += lengths[i];
    }
    rb = buffer;
    return tot;
}


void FillBuffer(Buffer &buff) {
    VeraCrypt::byte *b = buff.Ptr();
    for (auto i = 0; i < buff.Size(); ++i) {
        b[i] = i+1;
    }
}

void EmptyTest(shared_ptr<TestResult> r) {
    PipelineStream s;

    Buffer buff(1);
    size_t n = s.Read(buff);

    r->Info("N" + to_string(n));
    if (n != 0) {
        r->Failed("read some data, expected none");
    }
}

void ZeroLengthStreamsTest(shared_ptr<TestResult> r) {
    Buffer buff(1);
    FillBuffer(buff);

    auto m = make_shared<MemoryStream>(buff);

    MK(PipelineStream, s);
    s->AddStream(m);

    Buffer *rb;
    size_t n = ReadFully(s, rb);
    r->Info("N" + to_string(n));
    if (n != 1) {
        r->Failed("Expected 10 bytes");
    }

}

void MultipleZeroLengthStreams(shared_ptr<TestResult> r) {
    Buffer buf1(1);
    Buffer buf2(1);

    FillBuffer(buf1);
    FillBuffer(buf2);

    auto m1 = make_shared<MemoryStream>(buf1);
    auto m2 = make_shared<MemoryStream>(buf2);

    MK(PipelineStream, s);
    s->AddStream(m1);
    s->AddStream(m2);


    Buffer *rb;
    size_t n = ReadFully(s, rb);
    r->Info("N" + to_string(n));
    if (n != 2) {
        r->Failed("Expected 2 bytes");
    }

}



void LastZeroLengthStreamTest(shared_ptr<TestResult> r) {
    Buffer buf1(10);
    Buffer buf2(1);
    FillBuffer(buf1);
    FillBuffer(buf2);

    auto m1 = make_shared<MemoryStream>(buf1);
    auto m2 = make_shared<MemoryStream>(buf2);

    MK(PipelineStream,s);
    s->AddStream(m1);
    s->AddStream(m2);

    Buffer *rb;
    size_t n = ReadFully(s, rb);
    r->Info("N" + to_string(n));
    if (n != 11) {
        r->Failed("Expected 11 bytes");
    }
}

void ReadWholeSubstreamAtOnceTest(shared_ptr<TestResult> r) {
    Buffer buf1(10);
    Buffer buf2(5);
    FillBuffer(buf1);
    FillBuffer(buf2);

    auto m1 = make_shared<MemoryStream>(buf1);
    auto m2 = make_shared<MemoryStream>(buf2);

    MK(PipelineStream, s);
    s->AddStream(m1);
    s->AddStream(m2);

    Buffer *rb;
    size_t n = ReadFully(s, rb, 20);
    r->Info("N" + to_string(n));
    if (n != 15) {
        r->Failed("Expected 10 bytes");
    }
}

void ReadSubstreamByPartsTest(shared_ptr<TestResult> r) {
    Buffer buf1(10);
    Buffer buf2(1);
    FillBuffer(buf1);
    FillBuffer(buf2);

    auto m1 = make_shared<MemoryStream>(buf1);
    auto m2 = make_shared<MemoryStream>(buf2);

    MK(PipelineStream, s);
    s->AddStream(m1);
    s->AddStream(m2);

    Buffer *rb;
    size_t n = ReadFully(s, rb, 1);
    r->Info("N" + to_string(n));
    if (n != 11) {
        r->Failed("Expected 10 bytes");
    }
}

int main() {
    VeraCrypt::Testing t;
    t.AddTest("empty", &EmptyTest);
    t.AddTest("single zero length streams", &ZeroLengthStreamsTest);
    t.AddTest("multiple zero length stream", &MultipleZeroLengthStreams);
    t.AddTest("last zero length stream", &LastZeroLengthStreamTest);
    t.AddTest("read full", &ReadWholeSubstreamAtOnceTest);
    t.AddTest("read by single byte", &ReadSubstreamByPartsTest);
    t.Main();
};

