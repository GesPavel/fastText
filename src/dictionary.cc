/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "dictionary.h"
#include <assert.h>
#include <iostream>
#include <algorithm>
#include <iterator>
#include <unordered_map>
#include "args.h"

extern Args args;

const std::string Dictionary::EOS = "</s>";
const std::string Dictionary::BOW = "<";
const std::string Dictionary::EOW = ">";

Dictionary::Dictionary() {
  size_ = 0;
  nwords_ = 0;
  nlabels_ = 0;
  ntokens_ = 0;
  word2int_.resize(MAX_VOCAB_SIZE);
  for (int32_t i = 0; i < MAX_VOCAB_SIZE; i++) {
    word2int_[i] = -1;
  }
}

Dictionary::~Dictionary() {}

int32_t Dictionary::find(const std::string& w) {
  int32_t h = hash(w) % MAX_VOCAB_SIZE;
  while (word2int_[h] != -1 && words_[word2int_[h]].word != w) {
    h = (h + 1) % MAX_VOCAB_SIZE;
  }
  return h;
}

void Dictionary::add(const std::string& w) {
  int32_t h = find(w);
  ntokens_++;
  if (word2int_[h] == -1) {
    word2int_[h] = size_;
    entry we;
    we.word = w;
    we.uf = 1;
    we.type = (w.find(args.label) == 0) ? 1 : 0;
    words_.push_back(we);
    size_++;
  } else {
    words_[word2int_[h]].uf++;
  }
}

int32_t Dictionary::nwords() {
  return nwords_;
}

int32_t Dictionary::nlabels() {
  return nlabels_;
}

int64_t Dictionary::ntokens() {
  return ntokens_;
}

const std::vector<int32_t>& Dictionary::getNgrams(int32_t i) {
  assert(i >= 0 && i < nwords_);
  return words_[i].subwords;
}

const std::vector<int32_t> Dictionary::getNgrams(const std::string& word) {
  std::vector<int32_t> ngrams;
  int32_t i = getId(word);
  if (i >= 0) {
    ngrams = words_[i].subwords;
  } else {
    computeNgrams(BOW + word + EOW, ngrams);
  }
  return ngrams;
}

bool Dictionary::discard(int32_t id, real rand) {
  assert(id >= 0 && id < nwords_);
  if (args.model == model_name::sup) return false;
  return rand > pdiscard_[id];
}

int32_t Dictionary::getId(const std::string& w) {
  int32_t h = find(w);
  return word2int_[h];
}

int8_t Dictionary::getType(int32_t id) {
  assert(id >= 0 && id < size_);
  return words_[id].type;
}

std::string Dictionary::getWord(int32_t id) {
  assert(id >= 0 && id < size_);
  return words_[id].word;
}

uint32_t Dictionary::hash(const std::string& str) {
  uint32_t h = 2166136261;
  for (size_t i = 0; i < str.size(); i++) {
    h = h ^ uint32_t(str[i]);
    h = h * 16777619;
  }
  return h;
}

void Dictionary::computeNgrams(const std::string& word,
                               std::vector<int32_t>& ngrams) {
  for (size_t i = 0; i < word.size(); i++) {
    std::string ngram;
    if ((word[i] & 0xC0) == 0x80) continue;
    for (size_t j = i, n = 1; j < word.size() && n <= args.maxn; n++) {
      ngram.push_back(word[j++]);
      while (j < word.size() && (word[j] & 0xC0) == 0x80) {
        ngram.push_back(word[j++]);
      }
      if (n >= args.minn) {
        int32_t h = hash(ngram) % args.bucket;
        ngrams.push_back(nwords_ + h);
      }
    }
  }
}

void Dictionary::initNgrams() {
  for (size_t i = 0; i < size_; i++) {
    std::string word = BOW + words_[i].word + EOW;
    words_[i].subwords.push_back(i);
    computeNgrams(word, words_[i].subwords);
  }
}

std::string Dictionary::readWord(std::ifstream& fin)
{
  char c;
  std::string word;
  while (!fin.eof()) {
    fin.get(c);
    if (iswspace(c)) {
      if (word.empty()) {
        if (c == '\n') return EOS;
        continue;
      } else {
        if (c == '\n') fin.unget();
        return word;
      }
    }
    word.push_back(c);
  }
  return word;
}

void Dictionary::readFromFile(std::ifstream& ifs) {
  std::string word;
  int64_t minThreshold = 1;
  while (!(word = readWord(ifs)).empty()) {
    add(word);
    if (ntokens_ % 1000000 == 0) {
      std::cout << "\rRead " << ntokens_  / 1000000 << "M words" << std::flush;
    }
    if (size_ > 0.75 * MAX_VOCAB_SIZE) threshold(minThreshold++);
  }
  std::cout << "\rRead " << ntokens_  / 1000000 << "M words" << std::endl;
  threshold(args.minCount);
  initTableDiscard();
  initNgrams();
}

void Dictionary::threshold(int64_t t) {
  sort(words_.begin(), words_.end(), [](const entry& e1, const entry& e2) {
      if (e1.type != e2.type) return e1.type < e2.type;
      return e1.uf > e2.uf;
    });
  words_.erase(remove_if(words_.begin(), words_.end(), [&](const entry& e) {
        return e.uf < t;
      }), words_.end());
  words_.shrink_to_fit();

  size_ = 0;
  nwords_ = 0;
  nlabels_ = 0;
  for (int32_t i = 0; i < MAX_VOCAB_SIZE; i++) {
    word2int_[i] = -1;
  }
  for (auto it = words_.begin(); it != words_.end(); ++it) {
    int32_t h = find(it->word);
    word2int_[h] = size_;
    size_++;
    if (it->type == 0) nwords_++;
    if (it->type == 1) nlabels_++;
  }
}

void Dictionary::initTableDiscard() {
  pdiscard_.resize(size_);
  for (size_t i = 0; i < size_; i++) {
    real f = real(words_[i].uf) / real(ntokens_);
    pdiscard_[i] = sqrt(args.t / f) + args.t / f;
  }
}

std::vector<int64_t> Dictionary::getWordFreq() {
  std::vector<int64_t> freq;
  for (auto& w : words_) {
    if (w.type == 0) {
      freq.push_back(w.uf);
    }
  }
  return freq;
}

std::vector<int64_t> Dictionary::getLabelFreq() {
  std::vector<int64_t> freq;
  for (auto& w : words_) {
    if (w.type == 1) {
      freq.push_back(w.uf);
    }
  }
  return freq;
}

void Dictionary::addNgrams(std::vector<int32_t>& line, int32_t n) {
  int32_t line_size = line.size();
  for (int32_t i = 0; i < line_size; i++) {
    uint64_t h = line[i];
    for (int32_t j = i + 1; j < line_size && j < i + n; j++) {
      h = h * 116049371 + line[j];
      line.push_back(nwords_ + (h % args.bucket));
    }
  }
}

int32_t Dictionary::getLine(std::ifstream& ifs,
                            std::vector<int32_t>& words,
                            std::vector<int32_t>& labels,
                            std::minstd_rand& rng) {
  std::uniform_real_distribution<> uniform(0, 1);
  std::string token;
  int32_t ntokens = 0;
  words.clear();
  labels.clear();
  if (ifs.eof()) {
    ifs.clear();
    ifs.seekg(std::streampos(0));
  }
  while (!(token = readWord(ifs)).empty()) {
    if (token == EOS) break;
    int32_t wid = getId(token);
    if (wid < 0) continue;
    int8_t type = getType(wid);
    ntokens++;
    if (type == 0 && !discard(wid, uniform(rng))) {
      words.push_back(wid);
    }
    if (type == 1) {
      labels.push_back(wid - nwords_);
    }
    if (words.size() > MAX_LINE_SIZE && args.model != model_name::sup) break;
  }
  return ntokens;
}

std::string Dictionary::getLabel(int32_t lid) {
  return words_[lid + nwords_].word;
}

void Dictionary::save(std::ofstream& ofs) {
  ofs.write((char*) &size_, sizeof(int32_t));
  ofs.write((char*) &nwords_, sizeof(int32_t));
  ofs.write((char*) &nlabels_, sizeof(int32_t));
  ofs.write((char*) &ntokens_, sizeof(int64_t));
  for (int32_t i = 0; i < size_; i++) {
    entry e = words_[i];
    ofs.write(e.word.data(), e.word.size() * sizeof(char));
    ofs.put(0);
    ofs.write((char*) &(e.uf), sizeof(int64_t));
    ofs.write((char*) &(e.type), sizeof(int8_t));
  }
}

void Dictionary::load(std::ifstream& ifs) {
  words_.clear();
  for (int32_t i = 0; i < MAX_VOCAB_SIZE; i++) {
    word2int_[i] = -1;
  }
  ifs.read((char*) &size_, sizeof(int32_t));
  ifs.read((char*) &nwords_, sizeof(int32_t));
  ifs.read((char*) &nlabels_, sizeof(int32_t));
  ifs.read((char*) &ntokens_, sizeof(int64_t));
  for (int32_t i = 0; i < size_; i++) {
    char c;
    entry e;
    while ((c = ifs.get()) != 0) {
      e.word.push_back(c);
    }
    ifs.read((char*) &e.uf, sizeof(int64_t));
    ifs.read((char*) &e.type, sizeof(int8_t));
    words_.push_back(e);
    word2int_[find(e.word)] = i;
  }
  initTableDiscard();
  initNgrams();
}