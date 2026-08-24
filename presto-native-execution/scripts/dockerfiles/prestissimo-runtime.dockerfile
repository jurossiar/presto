# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

ARG DEPENDENCY_IMAGE=presto/prestissimo-dependency:centos9
ARG BASE_IMAGE=quay.io/centos/centos:stream9
FROM ${DEPENDENCY_IMAGE} as prestissimo-image

ARG OSNAME=centos
ARG BUILD_TYPE=Release
ARG PRESTO_OPTIONAL_FEATURES=''
ARG EXTRA_CMAKE_FLAGS=''
ARG NUM_THREADS=8
ARG CUDA_ARCHITECTURES=70

ENV PROMPT_ALWAYS_RESPOND=n
ENV BUILD_BASE_DIR=_build
ENV BUILD_DIR=""

RUN mkdir -p /prestissimo /runtime-libraries
COPY . /prestissimo/
RUN --mount=type=cache,target=/root/.ccache,sharing=locked \
    /bin/bash -c 'if [[ "${EXTRA_CMAKE_FLAGS}" =~ -DPRESTO_ENABLE_CUDF=ON ]] || [[ ",${PRESTO_OPTIONAL_FEATURES}," =~ ,cudf, ]]; then unset CC; unset CXX; source /opt/rh/gcc-toolset-14/enable; fi && \
    PRESTO_OPTIONAL_FEATURES=${PRESTO_OPTIONAL_FEATURES} \
    EXTRA_CMAKE_FLAGS=${EXTRA_CMAKE_FLAGS} \
    NUM_THREADS=${NUM_THREADS} make --directory="/prestissimo/" cmake-and-build BUILD_TYPE=${BUILD_TYPE} BUILD_DIR=${BUILD_DIR} BUILD_BASE_DIR=${BUILD_BASE_DIR} && \
    ccache -sz -v'
RUN !(LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/usr/local/lib:/usr/local/lib64 ldd /prestissimo/${BUILD_BASE_DIR}/${BUILD_DIR}/presto_cpp/main/presto_server  | grep "not found" | grep -v "libnvidia-ml") && \
    LD_LIBRARY_PATH=${LD_LIBRARY_PATH}:/usr/local/lib:/usr/local/lib64 ldd /prestissimo/${BUILD_BASE_DIR}/${BUILD_DIR}/presto_cpp/main/presto_server | awk 'NF == 4 { system("cp " $3 " /runtime-libraries") }'

# Record the CUDA version the dependency image was built with, so the runtime
# stage installs a matching runtime. Last instruction of this stage on purpose:
# it must not invalidate the cached compile above.
RUN echo "${CUDA_VERSION}" > /cuda_version

#/////////////////////////////////////////////
#          prestissimo-runtime
#//////////////////////////////////////////////

FROM ${BASE_IMAGE}

ENV BUILD_BASE_DIR=_build
ENV BUILD_DIR=""

# cuDF reaches NVRTC and nvJitLink through dlopen, building the version suffix at
# run time, so neither shows up as a NEEDED entry of presto_server or libcudf and
# the ldd sweep above cannot copy them. Without this the worker starts and then
# fails once a query needs a JIT-compiled kernel. The NVIDIA container runtime
# injects the driver (libcuda, libnvidia-ml) but not the CUDA toolkit libraries,
# so they have to be in the image.
COPY --from=prestissimo-image /prestissimo/velox/scripts/ /tmp/scripts/
COPY --from=prestissimo-image /cuda_version /tmp/
RUN CUDA_VERSION=$(cat /tmp/cuda_version) && \
    source /tmp/scripts/setup-centos-adapters.sh && \
    install_cuda_runtime "${CUDA_VERSION}" && \
    dnf clean all && \
    rm -rf /var/cache/dnf /tmp/scripts /tmp/cuda_version
RUN echo "/usr/local/cuda/lib64" > /etc/ld.so.conf.d/cuda.conf

COPY --chmod=0775 --from=prestissimo-image /prestissimo/${BUILD_BASE_DIR}/${BUILD_DIR}/presto_cpp/main/presto_server /usr/bin/
COPY --chmod=0775 --from=prestissimo-image /runtime-libraries/* /usr/lib64/prestissimo-libs/
COPY --chmod=0755 ./etc /opt/presto-server/etc
COPY --chmod=0775 ./entrypoint.sh /opt/entrypoint.sh
RUN echo "/usr/lib64/prestissimo-libs" > /etc/ld.so.conf.d/prestissimo.conf && ldconfig

# cuDF JIT-compiles kernels with NVRTC using --pch, and NVRTC resolves the
# precompiled header name 'kernel.pch' against the process working directory. The
# default would be '/', which is not writable when the container runs as an
# arbitrary UID (OpenShift's restricted SCC), so every JIT compilation fails with
# "cannot open precompiled header file" and the operators that need it report
# 'Broken promise'. /tmp is writable for any UID and is already where libcudf
# keeps its kernel bundle. Nothing resolves paths relative to the working
# directory - entrypoint.sh passes --etc-dir absolute - so this is inert
# otherwise.
WORKDIR /tmp

ENTRYPOINT ["/opt/entrypoint.sh"]
