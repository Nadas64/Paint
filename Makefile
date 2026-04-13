CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -municode
LDFLAGS = -lgdiplus -lgdi32 -luser32

SRC = src/main.cpp
OBJ = $(SRC:.cpp=.o)

TARGET = app

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(OBJ) -o $(TARGET).exe $(LDFLAGS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	del /Q src\*.o $(TARGET).exe 2>nul || exit 0

run: $(TARGET)
	.\$(TARGET).exe
