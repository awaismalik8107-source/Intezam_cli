#include "cache_disk_io.hpp"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/disk_interface.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/hasher.hpp>
#include <libtorrent/peer_request.hpp>

#include <boost/asio/post.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
#include <new>
#include <stdexcept>
#include <system_error>

namespace lt = libtorrent;

namespace intezam {

void CacheDiskState::fail(const std::string& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = message;
}

std::string CacheDiskState::error() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

namespace {

class CacheDiskIO final : public lt::disk_interface, public lt::buffer_allocator_interface {
public:
    CacheDiskIO(lt::io_context& context, std::shared_ptr<PieceStore> store,
                PieceGeometry geometry, std::shared_ptr<CacheDiskState> state)
        : context_(context), store_(std::move(store)), geometry_(geometry), state_(std::move(state)) {}

    lt::storage_holder new_torrent(const lt::storage_params& params, const std::shared_ptr<void>&) override
    {
        if (files_ || params.files.piece_length() != geometry_.piece_length
            || params.files.total_size() != geometry_.torrent_size) {
            throw std::runtime_error("Cache backend accepts only its selected torrent.");
        }
        files_ = &params.files;
        ++state_->active_storages;
        return {lt::storage_index_t{0}, *this};
    }

    void remove_torrent(lt::storage_index_t) override
    {
        files_ = nullptr;
        --state_->active_storages;
    }

    void async_read(lt::storage_index_t, const lt::peer_request& request,
        std::function<void(lt::disk_buffer_holder, const lt::storage_error&)> handler, lt::disk_job_flags_t) override
    {
        std::unique_ptr<char[]> buffer;
        const auto error = perform(lt::operation_t::file_read, [&] {
            const auto bytes = store_->readRaw(static_cast<int>(request.piece), request.start, request.length);
            buffer = std::make_unique<char[]>(bytes.size());
            std::memcpy(buffer.get(), bytes.data(), bytes.size());
        });
        boost::asio::post(context_, [this, handler = std::move(handler), buffer = std::move(buffer), error]() mutable {
            handler(lt::disk_buffer_holder{*this, buffer.release()}, error);
        });
    }

    bool async_write(lt::storage_index_t, const lt::peer_request& request, const char* bytes,
        std::shared_ptr<lt::disk_observer>, std::function<void(const lt::storage_error&)> handler,
        lt::disk_job_flags_t) override
    {
        const auto error = perform(lt::operation_t::file_write, [&] {
            if (request.length < 0) throw std::out_of_range("Negative piece write size");
            store_->write(static_cast<int>(request.piece), request.start, bytes, request.length);
        });
        boost::asio::post(context_, [handler = std::move(handler), error] { handler(error); });
        return false; // synchronous bounded writes: no unbounded pending disk queue
    }

    void async_hash(lt::storage_index_t, lt::piece_index_t piece, lt::span<lt::sha256_hash> v2,
        lt::disk_job_flags_t flags,
        std::function<void(lt::piece_index_t, const lt::sha1_hash&, const lt::storage_error&)> handler) override
    {
        lt::sha1_hash hash;
        const auto error = perform(lt::operation_t::file_read, [&] {
            const int index = static_cast<int>(piece);
            lt::hasher sha1;
            const int size = geometry_.pieceSize(index);
            const int v2_size = v2.empty() ? 0 : files_->piece_size2(piece);
            for (int offset = 0; offset < size; offset += lt::default_block_size) {
                const int length = std::min(lt::default_block_size, size - offset);
                const auto bytes = store_->readRaw(index, offset, length);
                if (flags & lt::disk_interface::v1_hash) sha1.update(lt::span<const char>{bytes});
                if (offset < v2_size) {
                    const int block = offset / lt::default_block_size;
                    if (block >= v2.size()) throw std::out_of_range("v2 hash output bounds");
                    v2[block] = lt::hasher256(bytes.data(), std::min(length, v2_size - offset)).final();
                }
            }
            if (flags & lt::disk_interface::v1_hash) hash = sha1.final();
        });
        boost::asio::post(context_, [handler = std::move(handler), piece, hash, error] { handler(piece, hash, error); });
    }

    void async_hash2(lt::storage_index_t, lt::piece_index_t piece, int offset, lt::disk_job_flags_t,
        std::function<void(lt::piece_index_t, const lt::sha256_hash&, const lt::storage_error&)> handler) override
    {
        lt::sha256_hash hash;
        const auto error = perform(lt::operation_t::file_read, [&] {
            const int size = files_->piece_size2(piece);
            if (offset < 0 || offset >= size) throw std::out_of_range("v2 block hash bounds");
            const auto bytes = store_->readRaw(static_cast<int>(piece), offset, std::min(lt::default_block_size, size - offset));
            hash = lt::hasher256(lt::span<const char>{bytes}).final();
        });
        boost::asio::post(context_, [handler = std::move(handler), piece, hash, error] { handler(piece, hash, error); });
    }

    void async_check_files(lt::storage_index_t, const lt::add_torrent_params* resume,
        lt::aux::vector<std::string, lt::file_index_t>,
        std::function<void(lt::status_t, const lt::storage_error&)> handler) override
    {
        const auto error = perform(lt::operation_t::check_resume, [&] {
            if (!resume) return;
            for (int piece = 0; piece < resume->have_pieces.size(); ++piece) {
                if (resume->have_pieces[lt::piece_index_t{piece}] && !store_->isVerified(piece)) {
                    throw std::runtime_error("Resume data claims a nonresident or unverified cache piece.");
                }
            }
        });
        boost::asio::post(context_, [handler = std::move(handler), error] {
            handler(error ? lt::disk_status::fatal_disk_error : lt::status_t{}, error);
        });
    }

    void async_clear_piece(lt::storage_index_t, lt::piece_index_t piece,
        std::function<void(lt::piece_index_t)> handler) override
    {
        store_->invalidate(static_cast<int>(piece));
        boost::asio::post(context_, [handler = std::move(handler), piece] { handler(piece); });
    }

    void async_release_files(lt::storage_index_t, std::function<void()> handler) override
    {
        if (handler) boost::asio::post(context_, std::move(handler));
    }
    void async_stop_torrent(lt::storage_index_t storage, std::function<void()> handler) override
    { async_release_files(storage, std::move(handler)); }

    void async_set_file_priority(lt::storage_index_t,
        lt::aux::vector<lt::download_priority_t, lt::file_index_t> priorities,
        std::function<void(const lt::storage_error&, lt::aux::vector<lt::download_priority_t, lt::file_index_t>)> handler) override
    {
        boost::asio::post(context_, [handler = std::move(handler), priorities = std::move(priorities)]() mutable {
            handler({}, std::move(priorities));
        });
    }
    void async_delete_files(lt::storage_index_t, lt::remove_flags_t,
        std::function<void(const lt::storage_error&)> handler) override
    {
        // Session cleanup owns the one cache file; active read leases extend its lifetime.
        boost::asio::post(context_, [handler = std::move(handler)] { handler({}); });
    }
    void async_move_storage(lt::storage_index_t, std::string path, lt::move_flags_t,
        std::function<void(lt::status_t, const std::string&, const lt::storage_error&)> handler) override
    {
        boost::asio::post(context_, [handler = std::move(handler), path = std::move(path)] {
            handler(lt::disk_status::fatal_disk_error, path, unsupported());
        });
    }
    void async_rename_file(lt::storage_index_t, lt::file_index_t file, std::string name,
        std::function<void(const std::string&, lt::file_index_t, const lt::storage_error&)> handler) override
    {
        boost::asio::post(context_, [handler = std::move(handler), file, name = std::move(name)] {
            handler(name, file, unsupported());
        });
    }

    void free_disk_buffer(char* buffer) override { delete[] buffer; }
    void free_multiple_buffers(lt::span<char*> buffers) override
    { for (auto* buffer : buffers) delete[] buffer; }
#if TORRENT_DEBUG_BUFFER_POOL
    void rename_buffer(char*, const char*) override {}
#endif
    void update_stats_counters(lt::counters&) const override {}
    std::vector<lt::open_file_state> get_status(lt::storage_index_t) const override { return {}; }
    void abort(bool) override {}
    void submit_jobs() override {}
    void settings_updated() override {}

private:
    static lt::storage_error unsupported()
    {
        return lt::storage_error{lt::error_code{boost::system::errc::operation_not_supported, lt::generic_category()}};
    }

    template<class Operation>
    lt::storage_error perform(lt::operation_t operation, Operation execute)
    {
        try {
            execute();
            return {};
        } catch (const std::system_error& error) {
            state_->fail(error.what());
            return {lt::error_code{error.code().value(), lt::generic_category()}, operation};
        } catch (const std::bad_alloc& error) {
            state_->fail(error.what());
            return {lt::error_code{boost::system::errc::not_enough_memory, lt::generic_category()}, operation};
        } catch (const std::exception& error) {
            state_->fail(error.what());
            return {lt::error_code{boost::system::errc::io_error, lt::generic_category()}, operation};
        }
    }

    lt::io_context& context_;
    std::shared_ptr<PieceStore> store_;
    PieceGeometry geometry_;
    std::shared_ptr<CacheDiskState> state_;
    const lt::file_storage* files_ = nullptr;
};

} // namespace

lt::disk_io_constructor_type cacheDiskConstructor(
    std::shared_ptr<PieceStore> store, PieceGeometry geometry, std::shared_ptr<CacheDiskState> state)
{
    return [store = std::move(store), geometry, state = std::move(state)](
        lt::io_context& context, const lt::settings_interface&, lt::counters&) {
        return std::make_unique<CacheDiskIO>(context, store, geometry, state);
    };
}

} // namespace intezam
