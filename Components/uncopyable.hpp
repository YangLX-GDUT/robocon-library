#ifndef COMPONENTS_UNCOPYABLE_HPP
#define COMPONENTS_UNCOPYABLE_HPP

namespace gdut {

class uncopyable {
public:
  uncopyable(const uncopyable &) = delete;
  uncopyable &operator=(const uncopyable &) = delete;

protected:
  uncopyable() = default;
  ~uncopyable() = default;
};

} // namespace gdut

#endif // COMPONENTS_UNCOPYABLE_HPP
