/*
 * Copyright 2016 neurodata (http://neurodata.io/)
 * Written by Disa Mhembere (disa@jhu.edu)
 *
 * This file is part of k-par-means
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY CURRENT_KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __KPM_I0_HPP__
#define __KPM_I0_HPP__

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <omp.h>
#include <assert.h>

#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>

namespace kpmeans { namespace base {

/* \Internal
 * \brief print a col wise matrix of type double / double.
 * Used for testing only.
 * \param matrix The col wise matrix.
 * \param rows The number of rows in the mat
 * \param cols The number of cols in the mat
 */
template <typename T>
void print_mat(T* matrix, const unsigned rows, const unsigned cols) {
    for (unsigned row = 0; row < rows; row++) {
        std::cout << "[";
        for (unsigned col = 0; col < cols; col++) {
            std::cout << " " << matrix[row*cols + col];
        }
        std::cout <<  " ]\n";
    }
}

template <typename T>
void print_arr(const T* arr, const unsigned len) {
    printf("[ ");
    for (unsigned i = 0; i < len; i++) {
        std::cout << arr[i] << " ";
    }
    printf("]\n");
}

template <typename T>
void print_vector(typename std::vector<T> v, unsigned max_print=100) {
    unsigned print_len = v.size() > max_print ? max_print : v.size();

    std::cout << "[";
    typename std::vector<T>::iterator itr = v.begin();
    for (; itr != v.begin()+print_len; itr++) {
        std::cout << " "<< *itr;
    }

    if (v.size() > print_len) std::cout << " ...";
    std::cout <<  " ]\n";
}

template <typename T>
class reader {
private:
    std::string fn;

protected:
    std::ifstream f;
    size_t ncol, nrow;

public:
    reader(const std::string fn) {
        this->fn = fn;
        ncol = 0;
        nrow = 0;
    }

    virtual void read(std::vector<T>& data) = 0;
    virtual bool readline(std::vector<T>& data) = 0;
    virtual void open() = 0;

    const std::string get_fn() const {
        return this->fn;
    }

    void set_fn(const std::string fn) {
        this->fn = fn;
    }

    const size_t get_nrow() const {
        return nrow;
    }

    const size_t get_ncol() const {
        return ncol;
    }

    void set_nrow(const size_t nrow) {
        this->nrow = nrow;
    }

    void set_ncol(const size_t ncol) {
        this->ncol = ncol;
    }

    ~reader() {
        f.close();
    }
};

template <typename T>
class text_reader : public reader<T> {
public:
    text_reader(const std::string fn) : reader<T>(fn) {
        this->open();
    }

    void read(std::vector<T>& data) override {
        assert(data.size() > 0);

        std::string line;
        size_t pos = 0;
        while (std::getline(this->f, line)) {
            T number;
            std::stringstream ss(line);

            while (ss >> number)
                data[pos++] = number;

            this->nrow++;
        }
    }

    void open() override {
        this->f.open(this->get_fn(), std::ios::in);
        assert(this->f.good());
    }

    bool readline(std::vector<T>& data) override {
        assert(bool(data.size()));
        std::string line;
        size_t pos = 0;
        if (std::getline(this->f, line)) {
            T number;
            std::stringstream ss(line);

            while (ss >> number)
                data[pos++] = number;

            this->nrow++;
            return true;
        }
        return false;
    }
};

template <typename T>
class bin_rm_reader : public reader<T> {
public:
    bin_rm_reader(const std::string fn) : reader<T>(fn) {
        this->open();
    }

    void seek(const size_t nbytes) {
        this->f.seekg(nbytes);
    }

    void read(std::vector<T>& data) override {
        assert(data.size() > 0);
        this->f.read(reinterpret_cast<char*>(data.data()),
                data.size()*sizeof(T));
    }

    bool readline(std::vector<T>& data) override {
        if (this->ncol) {
            if (this->f.read(reinterpret_cast<char*>(data.data()),
                        data.size()*sizeof(T))) {
                this->nrow++;
                return true;
            }
        } else {
            std::cout << "ncol: " << this->ncol << "\n";
            fprintf(stderr, "Cannot read a line without `ncol`\n");
            assert(false);
        }
        return false;
    }

    void open() override {
        this->f.open(this->get_fn(), std::ios::in | std::ios::binary);
        assert(this->f.good());
    }
};

// A very C-style binary io module
template <typename T>
class bin_io {
    private:
        FILE* f;
        size_t nrow, ncol;

        void cat(const T* arr) {
            std::cout << "[ ";
            for (size_t i = 0; i < ncol; i++) {
                std::cout << arr[i] << " ";
            }
            std::cout << "]\n";
        }

    public:
        bin_io(const std::string fn, const std::string mode) {
            f = fopen(fn.c_str(), mode.c_str());
            assert(NULL != f);
        }

        bin_io(const std::string fn, const size_t nrow,
                const size_t ncol, const std::string mode="rb") :
            bin_io(fn, mode) {
            this->nrow = nrow;
            this->ncol = ncol;
        }

        // Read data and cat in a viewer friendly fashion
        void read_cat() {
            T arr [ncol];
            for (size_t i = 0; i < nrow; i++) {
                size_t num_read = fread(&arr[0], sizeof(T)*ncol, 1, f);
                assert(num_read == 1);
                cat(arr);
            }
        }

        std::vector<T> readline() {
            std::vector<T> v;
            v.resize(ncol);
            size_t num_read = fread(&v[0], sizeof(T)*ncol, 1, f);
            assert(num_read == 1);
            return v;
        }

        void readline(T* v) {
            size_t num_read = fread(&v[0], sizeof(T)*ncol, 1, f);
            assert(num_read == 1);
        }

        // Read all the data!
        void read(std::vector<T>* v) {
            size_t num_read = fread(&((*v)[0]), sizeof(T)*ncol*nrow, 1, f);
            assert(num_read == 1);
        }

        // Read all the data!
        void read(T* v) {
            size_t num_read = fread(&v[0], sizeof(T)*ncol*nrow, 1, f);
            assert(num_read == 1);
        }

        void write(const T* data, const size_t numel) {
            size_t num_put = fwrite(data, sizeof(T)*numel, 1, f);
            assert(num_put == 1);
        }

        void write(const std::vector<T>& data, const size_t numel) {
            write(&(data[0]), numel);
        }

        operator bool() const {
            return ftell(f) != nrow*ncol*sizeof(T);
        }

        ~bin_io() {
            fclose(f);
        }
};

/**
  * \Internal Store data corresponding to a cluster in human readable format.
  */
void store_cluster(const unsigned id, const double* data,
        const unsigned numel, const unsigned* cluster_assignments,
        const size_t nrow, const size_t ncol, const std::string dir);

} } // End namespace kpmeans, base

#if 0
ws::S3::S3Client s3Client;
GetObjectRequest getObjectRequest;
getObjectRequest.SetBucket("sample_bucket");
getObjectRequest.SetKey("sample_key");
getObjectRequest.SetResponseStreamFactory(
    [](){
        return Aws::New(ALLOCATION_TAG, DOWNLOADED_FILENAME,
            std::ios_base::out | std::ios_base::in | std::ios_base::trunc);
    });
auto getObjectOutcome = s3Client.GetObject(getObjectRequest);
if(getObjectOutcome.IsSuccess()) {
    std::cout << "File downloaded from S3 to location " << DOWNLOADED_FILENAME;
}
else {
    std::cout << "File download failed from s3 with error "
        << getObjectOutcome.GetError().GetMessage();
}
#endif

#endif
