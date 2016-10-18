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

#ifndef __KPM_EXCEPTIONS_HPP__
#define __KPM_EXCEPTIONS_HPP__

#include <exception>

namespace kpmeans { namespace base {

class not_implemented_exception : public runtime_error {
    public not_implemented_exception() {
        runtime_error("Method not Implemented!\n");
    }
}

class thread_exception: public exception {
    private:
        std::string msg = "kpm::pthread::thread_exception ==> ";

        virtual const char* what() const throw() {
            return this->msg.c_str();
        }

    public:
        thread_exception(const std::string msg) {
            this->msg += msg;
        }
};
} }
#endif // __KPM_EXCEPTIONS_HPP__
