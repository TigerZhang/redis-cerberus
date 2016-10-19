#ifndef __STEKIN_UTILITY_POINTER_H__
#define __STEKIN_UTILITY_POINTER_H__

#include <memory>
#include <string>

namespace util {

    struct id {
        explicit id(void const* i)
            : _id(i)
        {}

        std::string str() const;

        bool operator<(id const& rhs) const;
        bool operator==(id const& rhs) const;
        bool operator!=(id const& rhs) const;
    private:
        void const* const _id;
    };

    template <typename RawType>
    struct weak_pointer {
        typedef RawType value_type;
        typedef typename std::unique_ptr<RawType>::pointer pointer;

        explicit weak_pointer(std::weak_ptr<RawType> ptr)
        : _ptr(ptr.lock().get())
        {}

        explicit weak_pointer(pointer ptr)
            : _ptr(ptr)
        {}

        template <typename ConvertableType>
        weak_pointer(weak_pointer<ConvertableType> rhs)
            : _ptr(rhs.template convert<RawType>()._ptr)
        {}

        template <typename ConvertableType>
        weak_pointer operator=(weak_pointer<ConvertableType> rhs)
        {
            _ptr = rhs.template convert<RawType>()._ptr;
            return *this;
        }

        template <typename TargetType>
        weak_pointer<TargetType> convert() const
        {
            return weak_pointer<TargetType>(_ptr);
        }

        bool operator==(weak_pointer rhs) const
        {
            return *_ptr == *rhs._ptr;
        }

        bool is(value_type const* rhs) const
        {
            return _ptr == rhs;
        }

        bool is(weak_pointer rhs) const
        {
            return _ptr == rhs._ptr;
        }

        bool operator!=(weak_pointer rhs) const
        {
            return *_ptr != *rhs._ptr;
        }

        bool operator<(weak_pointer rhs) const
        {
            return *_ptr < *rhs._ptr;
        }

        bool nul() const
        {
            return nullptr == _ptr;
        }

        bool not_nul() const
        {
            return !nul();
        }

        pointer operator->() const
        {
            return _ptr;
        }

        pointer get() const {
            return _ptr;
        }

        util::id id() const
        {
            return util::id(_ptr);
        }

        void reset()
        {
            _ptr = nullptr;
        }
    private:
        pointer _ptr;

        explicit weak_pointer(int) = delete;
    };

    template <typename RawType>
    struct unique_pointer
        : std::unique_ptr<RawType>
    {
        typedef RawType value_type;
        typedef std::unique_ptr<RawType> base_type;
        typedef typename base_type::pointer pointer;
        typedef typename base_type::deleter_type deleter_type;

        explicit unique_pointer(pointer p)
            : base_type(p)
        {}

        template <typename ConvertableType>
        unique_pointer(unique_pointer<ConvertableType>&& rhs)
            : base_type(std::move(rhs))
        {}

        template <typename ConvertableType>
        unique_pointer& operator=(unique_pointer<ConvertableType>&& rhs)
        {
            base_type::operator=(std::move(rhs));
            return *this;
        }

        weak_pointer<RawType> operator*() const
        {
            return weak_pointer<RawType>(base_type::get());
        }

        util::id id() const
        {
            return util::id(base_type::get());
        }

        std::string str() const
        {
            return id().str();
        }

        bool nul() const
        {
            return nullptr == base_type::get();
        }

        bool not_nul() const
        {
            return !nul();
        }

        explicit unique_pointer(int) = delete;
//        pointer get() const = delete;
        explicit operator bool() const = delete;
    };

    template <typename RawType>
    unique_pointer<RawType> make_unique_ptr(RawType *ptr)
    {
        return unique_pointer<RawType>(ptr);
    }

    template <typename RawType>
    weak_pointer<RawType> make_weak_pointer(RawType &obj)
    {
        return weak_pointer<RawType>(&obj);
    }

}

#endif /* __STEKIN_UTILITY_POINTER_H__ */
