/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
* Copyright (C) The University of Tennessee and The University
*               of Tennessee Research Foundation. 2016. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "libperf_int.h"

extern "C" {
#include <ucs/debug/log.h>
#include <ucs/sys/math.h>
}
#include <ucs/sys/preprocessor.h>


template <ucx_perf_cmd_t CMD, ucx_perf_test_type_t TYPE, ucp_perf_datatype_t DATA, bool ONESIDED>
class ucp_perf_test_runner {
public:
    static const ucp_tag_t TAG = 0x1337a880u;

    typedef uint8_t psn_t;

    ucp_perf_test_runner(ucx_perf_context_t &perf) :
        m_perf(perf),
        m_outstanding(0),
        m_max_outstanding(m_perf.params.max_outstanding)

    {
        ucs_assert_always(m_max_outstanding > 0);
    }

    /**
     * Make ucp_dt_iov_t iov[msg_size_cnt] array with pointer elements to
     * original buffer
     */
    void ucp_perf_test_prepare_iov_buffer()
    {
        const size_t iovcnt    = m_perf.params.msg_size_cnt;
        size_t iov_length_it, iov_it;

        if (UCP_PERF_DATATYPE_IOV == DATA) {
            ucs_assert(NULL != m_perf.params.msg_size_list);
            ucs_assert(iovcnt > 0);

            iov_length_it = 0;
            for (iov_it = 0; iov_it < iovcnt; ++iov_it) {
                m_perf.ucp.iov[iov_it].buffer = (char *)m_perf.send_buffer +
                                                iov_length_it;
                m_perf.ucp.iov[iov_it].length = m_perf.params.msg_size_list[iov_it];

                if (m_perf.params.iov_stride) {
                    iov_length_it += m_perf.params.iov_stride;
                } else {
                    iov_length_it += m_perf.ucp.iov[iov_it].length;
                }
            }

            ucs_debug("IOV buffer filled by %lu slices with total length %lu",
                      iovcnt, iov_length_it);
        }
    }

    void UCS_F_ALWAYS_INLINE progress_responder() {
        if (!ONESIDED) {
            ucp_worker_progress(m_perf.ucp.worker);
        }
    }

    void UCS_F_ALWAYS_INLINE progress_requestor() {
        ucp_worker_progress(m_perf.ucp.worker);
    }

    ucs_status_t UCS_F_ALWAYS_INLINE wait(void *request, bool is_requestor)
    {
        if (ucs_likely(!UCS_PTR_IS_PTR(request))) {
            return UCS_PTR_STATUS(request);
        }

        while (!ucp_request_is_completed(request)) {
            if (is_requestor) {
                progress_requestor();
            } else {
                progress_responder();
            }
        }
        ucp_request_release(request);
        return UCS_OK;
    }

    ucs_status_t UCS_F_ALWAYS_INLINE
    send(ucp_ep_h ep, void *buffer, unsigned length, uint8_t sn,
         uint64_t remote_addr, ucp_rkey_h rkey)
    {
        void *request;
        ucp_datatype_t datatype = ucp_dt_make_contig(1);

        switch (CMD) {
        case UCX_PERF_CMD_TAG:
            if (UCP_PERF_DATATYPE_IOV == m_perf.params.ucp.datatype) {
                buffer   = m_perf.ucp.iov;
                length   = m_perf.params.msg_size_cnt;
                datatype = ucp_dt_make_iov();
            }
            request = ucp_tag_send_nb(ep, buffer, length, datatype, TAG,
                                      (ucp_send_callback_t)ucs_empty_function);
            return wait(request, true);
        case UCX_PERF_CMD_PUT:
            *((uint8_t*)buffer + length - 1) = sn;
            return ucp_put(ep, buffer, length, remote_addr, rkey);
        case UCX_PERF_CMD_GET:
            return ucp_get(ep, buffer, length, remote_addr, rkey);
        case UCX_PERF_CMD_ADD:
            if (length == sizeof(uint32_t)) {
                return ucp_atomic_add32(ep, 1, remote_addr, rkey);
            } else if (length == sizeof(uint64_t)) {
                return ucp_atomic_add64(ep, 1, remote_addr, rkey);
            } else {
                return UCS_ERR_INVALID_PARAM;
            }
        case UCX_PERF_CMD_FADD:
            if (length == sizeof(uint32_t)) {
                return ucp_atomic_fadd32(ep, 0, remote_addr, rkey, (uint32_t*)buffer);
            } else if (length == sizeof(uint64_t)) {
                return ucp_atomic_fadd64(ep, 0, remote_addr, rkey, (uint64_t*)buffer);
            } else {
                return UCS_ERR_INVALID_PARAM;
            }
        case UCX_PERF_CMD_SWAP:
            if (length == sizeof(uint32_t)) {
                return ucp_atomic_swap32(ep, 0, remote_addr, rkey, (uint32_t*)buffer);
            } else if (length == sizeof(uint64_t)) {
                return ucp_atomic_swap64(ep, 0, remote_addr, rkey, (uint64_t*)buffer);
            } else {
                return UCS_ERR_INVALID_PARAM;
            }
        case UCX_PERF_CMD_CSWAP:
            if (length == sizeof(uint32_t)) {
                return ucp_atomic_cswap32(ep, 0, 0, remote_addr, rkey, (uint32_t*)buffer);
            } else if (length == sizeof(uint64_t)) {
                return ucp_atomic_cswap64(ep, 0, 0, remote_addr, rkey, (uint64_t*)buffer);
            } else {
                return UCS_ERR_INVALID_PARAM;
            }
        default:
            return UCS_ERR_INVALID_PARAM;
        }
    }

    ucs_status_t UCS_F_ALWAYS_INLINE
    recv(ucp_worker_h worker, void *buffer, unsigned length, uint8_t sn)
    {
        volatile uint8_t *ptr;
        void *request;

        switch (CMD) {
        case UCX_PERF_CMD_TAG:
            request = ucp_tag_recv_nb(worker, buffer, length, ucp_dt_make_contig(1),
                                      TAG, 0,
                                      (ucp_tag_recv_callback_t)ucs_empty_function);
            return wait(request, false);
        case UCX_PERF_CMD_PUT:
            switch (TYPE) {
            case UCX_PERF_TEST_TYPE_PINGPONG:
                ptr = (volatile uint8_t*)buffer + length - 1;
                while (*ptr != sn) {
                    progress_responder();
                }
                return UCS_OK;
            case UCX_PERF_TEST_TYPE_STREAM_UNI:
                return UCS_OK;
            default:
                return UCS_ERR_INVALID_PARAM;
            }
        case UCX_PERF_CMD_GET:
        case UCX_PERF_CMD_ADD:
        case UCX_PERF_CMD_FADD:
        case UCX_PERF_CMD_SWAP:
        case UCX_PERF_CMD_CSWAP:
            switch (TYPE) {
            case UCX_PERF_TEST_TYPE_STREAM_UNI:
                progress_responder();
                return UCS_OK;
            default:
                return UCS_ERR_INVALID_PARAM;
            }
        default:
            return UCS_ERR_INVALID_PARAM;
        }
    }

    ucs_status_t run_pingpong()
    {
        unsigned my_index;
        ucp_worker_h worker;
        ucp_ep_h ep;
        void *send_buffer, *recv_buffer;
        uint64_t remote_addr;
        uint8_t sn;
        ucp_rkey_h rkey;
        size_t length;

        length = ucx_perf_get_message_size(&m_perf.params);
        ucs_assert(length >= sizeof(psn_t));

        ucp_perf_test_prepare_iov_buffer();

        *((volatile uint8_t*)m_perf.recv_buffer + length - 1) = -1;

        rte_call(&m_perf, barrier);

        my_index = rte_call(&m_perf, group_index);

        ucx_perf_test_start_clock(&m_perf);

        send_buffer = m_perf.send_buffer;
        recv_buffer = m_perf.recv_buffer;
        worker      = m_perf.ucp.worker;
        ep          = m_perf.ucp.peers[1 - my_index].ep;
        remote_addr = m_perf.ucp.peers[1 - my_index].remote_addr + m_perf.offset;
        rkey        = m_perf.ucp.peers[1 - my_index].rkey;
        sn          = 0;

        if (my_index == 0) {
            UCX_PERF_TEST_FOREACH(&m_perf) {
                send(ep, send_buffer, length, sn, remote_addr, rkey);
                recv(worker, recv_buffer, length, sn);
                ucx_perf_update(&m_perf, 1, length);
                ++sn;
            }
        } else if (my_index == 1) {
            UCX_PERF_TEST_FOREACH(&m_perf) {
                recv(worker, recv_buffer, length, sn);
                send(ep, send_buffer, length, sn, remote_addr, rkey);
                ucx_perf_update(&m_perf, 1, length);
                ++sn;
            }
        }

        ucp_worker_flush(m_perf.ucp.worker);
        rte_call(&m_perf, barrier);
        return UCS_OK;
    }

    ucs_status_t run_stream_uni()
    {
        unsigned my_index;
        ucp_worker_h worker;
        ucp_ep_h ep;
        void *send_buffer, *recv_buffer;
        uint64_t remote_addr;
        ucp_rkey_h rkey;
        size_t length;
        uint8_t sn;

        length = ucx_perf_get_message_size(&m_perf.params);
        ucs_assert(length >= sizeof(psn_t));

        ucp_perf_test_prepare_iov_buffer();

        rte_call(&m_perf, barrier);

        my_index = rte_call(&m_perf, group_index);

        ucx_perf_test_start_clock(&m_perf);

        send_buffer = m_perf.send_buffer;
        recv_buffer = m_perf.recv_buffer;
        worker      = m_perf.ucp.worker;
        ep          = m_perf.ucp.peers[1 - my_index].ep;
        remote_addr = m_perf.ucp.peers[1 - my_index].remote_addr + m_perf.offset;
        rkey        = m_perf.ucp.peers[1 - my_index].rkey;
        sn          = 0;

        if (my_index == 0) {
            UCX_PERF_TEST_FOREACH(&m_perf) {
                recv(worker, recv_buffer, length, sn);
                ucx_perf_update(&m_perf, 1, length);
                ++sn;
            }
        } else if (my_index == 1) {
            UCX_PERF_TEST_FOREACH(&m_perf) {
                send(ep, send_buffer, length, sn, remote_addr, rkey);
                ucx_perf_update(&m_perf, 1, length);
                ++sn;
            }
        }

        ucp_worker_flush(m_perf.ucp.worker);
        rte_call(&m_perf, barrier);
        return UCS_OK;
    }

    ucs_status_t run()
    {
        switch (TYPE) {
        case UCX_PERF_TEST_TYPE_PINGPONG:
            return run_pingpong();
        case UCX_PERF_TEST_TYPE_STREAM_UNI:
            return run_stream_uni();
        case UCX_PERF_TEST_TYPE_STREAM_BI:
        default:
            return UCS_ERR_INVALID_PARAM;
        }
    }

private:
    ucx_perf_context_t &m_perf;
    unsigned           m_outstanding;
    const unsigned     m_max_outstanding;
};


#define TEST_CASE(_perf, _cmd, _type, _data, _onesided) \
    if (((_perf)->params.command == (_cmd)) && \
        ((_perf)->params.test_type == (_type)) && \
        ((_perf)->params.ucp.datatype == (_data)) && \
        (!!((_perf)->params.flags & UCX_PERF_TEST_FLAG_ONE_SIDED) == !!(_onesided))) \
    { \
        ucp_perf_test_runner<_cmd, _type, _data, _onesided> r(*_perf); \
        return r.run(); \
    }
#define TEST_CASE_ALL_OSD(_perf, _case) \
   TEST_CASE(_perf, UCS_PP_TUPLE_0 _case, UCS_PP_TUPLE_1 _case, UCS_PP_TUPLE_2 _case, true) \
   TEST_CASE(_perf, UCS_PP_TUPLE_0 _case, UCS_PP_TUPLE_1 _case, UCS_PP_TUPLE_2 _case, false)


ucs_status_t ucp_perf_test_dispatch(ucx_perf_context_t *perf)
{
    UCS_PP_FOREACH(TEST_CASE_ALL_OSD, perf,
        (UCX_PERF_CMD_TAG,   UCX_PERF_TEST_TYPE_PINGPONG,   UCP_PERF_DATATYPE_CONTIG),
        (UCX_PERF_CMD_TAG,   UCX_PERF_TEST_TYPE_STREAM_UNI, UCP_PERF_DATATYPE_CONTIG),
        (UCX_PERF_CMD_TAG,   UCX_PERF_TEST_TYPE_PINGPONG,   UCP_PERF_DATATYPE_IOV),
        (UCX_PERF_CMD_TAG,   UCX_PERF_TEST_TYPE_STREAM_UNI, UCP_PERF_DATATYPE_IOV),
        (UCX_PERF_CMD_PUT,   UCX_PERF_TEST_TYPE_PINGPONG,   UCP_PERF_DATATYPE_CONTIG),
        (UCX_PERF_CMD_PUT,   UCX_PERF_TEST_TYPE_STREAM_UNI, UCP_PERF_DATATYPE_CONTIG),
        (UCX_PERF_CMD_GET,   UCX_PERF_TEST_TYPE_STREAM_UNI, UCP_PERF_DATATYPE_CONTIG),
        (UCX_PERF_CMD_ADD,   UCX_PERF_TEST_TYPE_STREAM_UNI, UCP_PERF_DATATYPE_CONTIG),
        (UCX_PERF_CMD_FADD,  UCX_PERF_TEST_TYPE_STREAM_UNI, UCP_PERF_DATATYPE_CONTIG),
        (UCX_PERF_CMD_SWAP,  UCX_PERF_TEST_TYPE_STREAM_UNI, UCP_PERF_DATATYPE_CONTIG),
        (UCX_PERF_CMD_CSWAP, UCX_PERF_TEST_TYPE_STREAM_UNI, UCP_PERF_DATATYPE_CONTIG)
        );

    ucs_error("Invalid test case");
    return UCS_ERR_INVALID_PARAM;
}
