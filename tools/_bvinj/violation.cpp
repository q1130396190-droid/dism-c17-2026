#include <windows.h>
// by-value parameter: would copy the object
void TakeIt(CActiveXCtrl ctrl);
// by-value local: would copy on construction
void MakeOne() { CActiveXCtrl local; }
#include <vector>
// container element: requires copy
// std::vector<CActiveXCtrl> v;
