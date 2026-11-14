#include <iostream>
#include <string>
#include <vector>

int main(){
    std::vector <int> ids;
    std::string lines;
    std::getline (std::cin,lines);

    ids.reserve(lines.size());

    for(unsigned char line:lines){

        ids.push_back(static_cast<int>(line));

    }

    for(std::vector<int>::size_type i = 0 ; i+1 < ids.size();i++){

        std::cout << ids[i] <<' '<< ids[i+1] << '\n';


    }

    return 0;
}