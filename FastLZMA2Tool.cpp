/*
* Copyright (c) 2018, Conor McCarthy
* All rights reserved.
*
* This source code is licensed under both the BSD-style license (found in the
* LICENSE file in the root directory of this source tree) and the GPLv2 (found
* in the COPYING file in the root directory of this source tree).
* You may select, at your option, one of the above-listed licenses.
*/

/*
* Modified by A2K in 2023
*/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <iostream>
#include <fstream>
#include <chrono>
#include <filesystem>
#include "fastlzma2/fast-lzma2.h" /* Assumes that libfast-lzma2 was installed using 'make install' */

#define VERSION "1.0.0"

static int thread_count = 8;
static std::ifstream fin;
static std::ofstream fout;
static std::string out_name;
static FL2_CStream* fcs;
static FL2_DStream* fds;

static void exit_fail(const char* msg)
{
  fputs(msg, stderr);
  exit(1);
}

template<typename T = char>
static size_t ffread(T* buffer, size_t size)
{
   return fin.read((char*)buffer, size).gcount();
}

template<typename T = char>
static size_t ffwrite(T* buffer, size_t size)
{
  fout.write((char*)buffer, size);
  return size;
}

struct Timer {
  std::chrono::steady_clock::time_point begin;
  const char* prefix;
  Timer(const char* inPrefix) : begin(std::chrono::steady_clock::now()), prefix(inPrefix) {};
  ~Timer() {
    std::cout << prefix << " took "
      << std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - begin).count()
      << "s" << std::endl;
  }
};

static int compress_file(FL2_CStream* fcs)
{
  Timer timer("Compression");
  unsigned char in_buffer[8 * 1024];
  unsigned char out_buffer[4 * 1024];
  FL2_inBuffer in_buf = { in_buffer, sizeof(in_buffer), sizeof(in_buffer) };
  FL2_outBuffer out_buf = { out_buffer, sizeof(out_buffer), 0 };
  size_t res = 0;
  size_t in_size = 0;
  size_t out_size = 0;
  do {
    if (in_buf.pos == in_buf.size) {
      in_buf.size = ffread(in_buffer, sizeof(in_buffer));
      in_size += in_buf.size;
      in_buf.pos = 0;
    }
    res = FL2_compressStream(fcs, &out_buf, &in_buf);
    if (FL2_isError(res))
      goto error_out;

    ffwrite(out_buffer, out_buf.pos);

    out_size += out_buf.pos;
    out_buf.pos = 0;

  } while (in_buf.size);
  do {
    res = FL2_endStream(fcs, &out_buf);
    if (FL2_isError(res))
      goto error_out;

    ffwrite(out_buf.dst, out_buf.pos);
    out_size += out_buf.pos;
    out_buf.pos = 0;
  } while (res);
  fprintf(stdout, "Compressed: %lld -> %lld\n", in_size, out_size);

  return 0;

error_out:
  fprintf(stderr, "Error: %s\n", FL2_getErrorName(res));
  return 1;
}

static int decompress_file(FL2_DStream* fds)
{
  Timer timer("Decompression");
  unsigned char in_buffer[4 * 1024];
  unsigned char out_buffer[8 * 1024];
  FL2_inBuffer in_buf = { in_buffer, sizeof(in_buffer), sizeof(in_buffer) };
  FL2_outBuffer out_buf = { out_buffer, sizeof(out_buffer), 0 };
  size_t res;
  size_t in_size = 0;
  size_t out_size = 0;
  do {
    if (in_buf.pos == in_buf.size) {
      in_buf.size = ffread(in_buffer, sizeof in_buffer);
      in_size += in_buf.size;
      in_buf.pos = 0;
    }
    res = FL2_decompressStream(fds, &out_buf, &in_buf);
    if (FL2_isError(res))
      goto error_out;
    ffwrite(out_buf.dst, out_buf.pos);
    /* Discard the output. XXhash will verify the integrity. */
    out_size += out_buf.pos;
    out_buf.pos = 0;
  } while (res && in_buf.size);

  fprintf(stdout, "\tDecompressed: %lld -> %lld\n", in_size, out_size);

  return 0;

error_out:
  fprintf(stderr, "Error: %s\n", FL2_getErrorName(res));
  return 1;
}

bool bCompress = true;
static void open_files(const char* name)
{
  fin = std::ifstream(name, std::ios_base::binary);
  if (!fin.is_open())
    exit_fail("Cannot open input file.\n");
  fin.seekg(0, fin.beg);

  fout = std::ofstream(out_name, std::ios_base::binary);
  if (!fout.is_open())
    exit_fail("Cannot open output file.\n");
}

static void create_init_fl2_streams(int preset)
{  
  fcs = FL2_createCStreamMt(thread_count, 0);
  if (fcs == NULL)
    exit_fail("Cannot allocate compression context.\n");

  fds = FL2_createDStreamMt(thread_count);
  if (fds == NULL)
    exit_fail("Cannot allocate decompression context.\n");

  size_t res = FL2_initCStream(fcs, preset);
  if (!res)
    res = FL2_initDStream(fds);
  if (FL2_isError(res)) {
    fprintf(stderr, "Error: %s\n", FL2_getErrorName(res));
    exit(1);
  }
}

int main(int argc, char** argv)
{
  std::cout << "FastLZMA2Tool version " VERSION << std::endl;
  int ret = 0;
  static const char* usage = "Usage: [compress|decompress] [-preset=1..10] [-threads=N] FILE[.lzma2]";

  if (argc < 2)
    exit_fail(usage);

  int argIndex = 1;

  const char* op = argv[argIndex];
  if (strncmp(op, "compress", strlen("compress")) == 0)
  {
    bCompress = true;
    argIndex++;
  }
  else if (strncmp(op, "decompress", strlen("decompress")) == 0)
  {
    bCompress = false;
    argIndex++;
  }

  int preset = 6;

  struct {
    const std::string threads = "-threads=";
    const std::string preset = "-preset=";
  } static args;
  for (int i = argIndex; i < argc; ++i)
  {
    const std::string arg(argv[i]);
    if (arg.find(args.threads) == 0)
    {
      thread_count = std::atoi(arg.substr(args.threads.size()).c_str());
    }
    else if (arg.find(args.preset) == 0)
    {
      preset = std::atoi(arg.substr(args.preset.size()).c_str());
    }
  }

  std::cout << "Mode: " << (bCompress ? "COMPRESS" : "DECOMPRESS") << std::endl;
  std::cout << "Compression level: " << preset << std::endl;
  std::cout << "Threads: " << thread_count << std::endl;

  const char* name = NULL;
  for (int i = argIndex; i < argc; ++i)
    if (argv[i][0] != '-') {
      name = argv[i];
      if (i + 1 < argc)
      {
        out_name = std::string(argv[i + 1], strlen(argv[i+1]));
        std::cout << "output name parsed: " << out_name << std::endl;
      }
      break;
    }
  if (name == NULL)
  {
    exit_fail(usage);
  }
  else if (!out_name.size())
  {
    out_name = name;
  }

  static const std::string postfix = ".lzma2";


  if (bCompress)
  {
    if (out_name.find(":\\") != 1 && out_name.find(":/") != 1)
    {
      out_name = std::filesystem::current_path().string() + "\\" + out_name;
    }
    if (!std::equal(postfix.rbegin(), postfix.rend(), out_name.rbegin()))
    {
      out_name += postfix;
    }
  }
  else
  {
    if (std::equal(postfix.rbegin(), postfix.rend(), out_name.rbegin()))
    {
      out_name = out_name.substr(0, out_name.size() - postfix.size());
    }
  }

  create_init_fl2_streams(preset);
  open_files(name);

  if (bCompress)
  {
    fprintf(stdout, "Compressing \n\  %s\n    to\n  %s\n", name, out_name.c_str());
    ret = compress_file(fcs);
    if (ret == 0) {
      fprintf(stdout, "Compression SUCCESS.\n");
    }
  }
  else
  {
    fprintf(stdout, "Decompressing to %s\n", out_name.c_str());
    ret = decompress_file(fds);
    if (ret == 0) {
      fprintf(stdout, "Decompression SUCCESS.\n");
    }
  }

  fout.flush();
  fout.close();
  fin.close();

  FL2_freeCStream(fcs);
  FL2_freeDStream(fds);
  return ret;
}