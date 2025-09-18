#ifndef _BANNERS_H_
#define _BANNERS_H_

#include <vector>
#include <string>

class Board;
class Banners {
public:
    Banners(Board* board) : board_(board), current_index_(0) {
    }

    virtual ~Banners() = default;

    bool Fetch();
    std::string Next();

private:
    Board* board_;
    std::vector<std::string> banners_;
    size_t current_index_;
};

#endif // _BANNERS_H_
