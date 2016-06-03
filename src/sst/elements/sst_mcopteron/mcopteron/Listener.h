// Copyright 2009-2016 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
// 
// Copyright (c) 2009-2016, Sandia Corporation
// All rights reserved.
// 
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.


#ifndef LISTENER
#define LISTENER

namespace McOpteron{ //Scoggin: Added a namespace to reduce possible conflicts as library
class Listener{
 public:
   virtual void notify(void *obj) {};
   virtual ~Listener() {};
};
}//End namespace McOpteron
#endif
