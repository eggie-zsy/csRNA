#include <iostream>
#include <string>
class father{
  public:
      //我的年龄公布与众,别人问我我会说，外人可以构造我
      int age;
      virtual void sayage(){std::cout << age << std::endl;}
      father(int age,int password,int addr)
        :age(age),password(password),addr(addr)
      {}
  private:      
      //我自己的密码，不能告诉任何人
      int password;
  protected:
      int addr;        
      //我只传授予自己的子孙，外人不得使用
      void saypass(){ std::cout << password<< std::endl;}
      void sayaddr(){ std::cout << addr << std::endl;}       

};
class son:public father{
  public: 
     //外人可以构造我
     son(int age, int password, int addr, int toy,father* fatherptr) 
        : father(age, password, addr), toy(toy),myfather(fatherptr) {}  
     //外人可以通过问我的年龄，我会说三遍
     void sayage() override{std::cout << age << age << age << std::endl;}
     //外人可以询问我爸的年龄，他的sayage是公共的，所以他会回答问题
     void saymyfatherage(){myfather->sayage();}
     //把传承下来的protected的函数变成public，不肖子孙
     void sayaddrpublic(){sayaddr();}
  private:
    //我的玩具数量和我爸是谁是秘密，不得告诉其他人
    int toy;
    father* myfather;
};
class grandson: public son{
  public :
    grandson(int age, int password, int addr, int toy,father* fatherptr):
    son(age,password,addr,toy,fatherptr){}
  
};
int main(){
   father afather(45,12345,429);
   father* fatherptr1 = NULL;
   father* fatherptr2 = NULL;
   fatherptr1=&afather;
   son ason(10, 54321, 429, 0, fatherptr1);  
   fatherptr2=&ason;
   grandson agrandson(3, 5432, 429, 1, fatherptr2);
  
   ason.saymyfatherage();
   afather.sayage();
   ason.sayaddrpublic();
   agrandson.saymyfatherage();
}