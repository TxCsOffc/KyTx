// Contracts exercised through the production exports and PM4 dispatch tables.
template<class Result,class... Args> Result CallPortExport(uint64_t entry,Args... args) {
#if KYTY_GUEST_CPU_EMULATED
  const auto word=[](auto arg)->uint64_t {
    if constexpr(std::is_pointer_v<decltype(arg)>) return reinterpret_cast<uintptr_t>(arg);
    else return static_cast<uint64_t>(arg);
  };
  const std::array<uint64_t,sizeof...(Args)> words{word(args)...};
  const auto result=Loader::GuestCpu::CallGuest(entry,words.data(),words.size());
  if constexpr(std::is_pointer_v<Result>) return reinterpret_cast<Result>(result);
  else return static_cast<Result>(result);
#else
  return reinterpret_cast<Result(KYTY_SYSV_ABI*)(Args...)>(entry)(args...);
#endif
}

void CheckOpenFileCoherence(const std::filesystem::path& root) {
  const auto path=root/"open-file-coherence.dat";
  Common::File writer;
  Check(writer.Create(path),"create shared file fixture");
  uint32_t count=0;
  writer.Write("abcdef",6,&count);
  Check(count==6 && Common::File::Size(path)==6,"new writes visible without flush/close");
  Common::File reader(path,Common::File::Mode::Read),update(path,Common::File::Mode::Write);
  Check(!reader.IsInvalid() && !update.IsInvalid(),"independent handles open");
  std::array<char,2> bytes{};
  reader.Read(bytes.data(),1,&count);
  Check(count==1 && bytes[0]=='a' && update.Seek(1),"prime sequential reader");
  update.Write("XY",2,&count);
  reader.Read(bytes.data(),2,&count);
  Check(count==2 && std::memcmp(bytes.data(),"XY",2)==0,"reader observes independent writer without seek");
  Common::File read_write(path,Common::File::Mode::ReadWrite);
  Check(!read_write.IsInvalid() && read_write.Seek(3),"read-write handle opens");
  read_write.Write("Z",1,&count); reader.Read(bytes.data(),1,&count);
  Check(count==1 && bytes[0]=='Z',"read-write updates are immediately visible");
  writer.Write("g",1,&count);
  std::error_code error; std::filesystem::resize_file(path,2,error);
  Check(!error && writer.Tell()==7,"truncate preserves open writer offset");
  writer.Close(); update.Close(); read_write.Close();
  Check(Common::File::Size(path)==2,"close cannot restore buffered data after truncate");
}

void TestFsyncAndRmdir(const std::filesystem::path& root) {
  Loader::SymbolDatabase symbols; Libs::InitLibKernel_1(&symbols);
  const auto* symbol=symbols.Find({"juWbTNM+8hw","Posix",1,"libkernel",1,1,Loader::SymbolType::Func});
  Check(symbol!=nullptr,"POSIX fsync export resolves");
  const auto sync=[&](int descriptor) { return CallPortExport<int>(symbol->vaddr,descriptor); };
  Check(sync(-1)==-1 && *Libs::Posix::GetErrorAddr()==Libs::Posix::POSIX_EBADF,"POSIX fsync reports EBADF");
  const int descriptor=FileSystem::KernelOpen("/savedata0/fsync.dat",0x602,0777);
  Check(descriptor>=3 && FileSystem::KernelWrite(descriptor,"save",4)==4 && sync(descriptor)==0 &&
        FileSystem::KernelLseek(descriptor,0,1)==4,"fsync flushes data and preserves offset");
  const int readonly=FileSystem::KernelOpen("/savedata0/fsync.dat",0,0);
  Check(readonly>=3 && FileSystem::KernelFsync(readonly)==OK,"read-only fsync flushes shared file");
  Check(FileSystem::KernelClose(readonly)==OK && FileSystem::KernelClose(descriptor)==OK && sync(descriptor)==-1 &&
        *Libs::Posix::GetErrorAddr()==Libs::Posix::POSIX_EBADF,"closed fsync descriptor rejected");
  std::filesystem::create_directory(root/"sync-dir");
  const int directory=FileSystem::KernelOpen("/savedata0/sync-dir",0x20000,0);
  Check(directory>=3 && FileSystem::KernelFsync(directory)==OK,"directory metadata is flushed");
  Check(FileSystem::KernelRename("/savedata0/sync-dir","/savedata0/renamed-sync-dir")==OK &&
        FileSystem::KernelFsync(directory)==OK,"renamed directory keeps its sync handle");
  Check(FileSystem::KernelClose(directory)==OK,"close synced directory");
  {
    Common::File file; Check(file.Create(root/"rename-target.dat"),"create rename target");
    file.Write("old",3);
  }
  Check(FileSystem::KernelRename("/savedata0/missing-source","/savedata0/rename-target.dat")==Libs::LibKernel::KERNEL_ERROR_ENOENT &&
        Common::File::Size(root/"rename-target.dat")==3,"failed rename preserves existing destination");
  Check(FileSystem::KernelRename("/savedata0/fsync.dat","/savedata0/rename-target.dat")==OK &&
        Common::File::Size(root/"rename-target.dat")==4,"rename replaces existing destination");
  Check(FileSystem::KernelRmdir(nullptr)==Libs::LibKernel::KERNEL_ERROR_EFAULT,"rmdir null path");
  Check(FileSystem::KernelRmdir("/savedata0/absent-dir")==Libs::LibKernel::KERNEL_ERROR_ENOENT,"rmdir missing path");
  Check(FileSystem::KernelRmdir("/savedata0/rename-target.dat")==Libs::LibKernel::KERNEL_ERROR_ENOTDIR,"rmdir regular file");
  Check(FileSystem::KernelRmdir("/savedata0///")==Libs::LibKernel::KERNEL_ERROR_EBUSY,"rmdir mount root protected");
  Check(FileSystem::KernelRmdir("/savedata0/.")==Libs::LibKernel::KERNEL_ERROR_EINVAL,"rmdir dot rejected");
  Check(FileSystem::KernelRmdir("/savedata0/..")==Libs::LibKernel::KERNEL_ERROR_EINVAL,"rmdir parent rejected");
  std::filesystem::create_directory(root/"nonempty-dir");
  { Common::File file; Check(file.Create(root/"nonempty-dir"/"item"),"create nonempty directory fixture"); }
  Check(FileSystem::KernelRmdir("/savedata0/nonempty-dir")==Libs::LibKernel::KERNEL_ERROR_ENOTEMPTY,"rmdir preserves nonempty directory");
  Check(FileSystem::KernelRmdir("/savedata0//renamed-sync-dir")==OK,"rmdir normalized empty directory");
  std::error_code error;
  std::filesystem::create_directory_symlink(root/"nonempty-dir",root/"dir-link",error);
  if(!error) {
    Check(FileSystem::KernelRmdir("/savedata0/dir-link")==Libs::LibKernel::KERNEL_ERROR_ENOTDIR &&
          std::filesystem::is_symlink(root/"dir-link"),"rmdir cannot delete a directory symlink");
  }
}

void TestLibcStringExports() {
  Loader::SymbolDatabase symbols; Libs::LibcInternal::InitLibcInternal_1(&symbols);
  const auto find=[&](const char* nid) {
    const auto* symbol=symbols.Find({nid,"LibcInternal",1,"LibcInternal",1,1,Loader::SymbolType::Func});
    Check(symbol!=nullptr,"libc string export resolves"); return symbol->vaddr;
  };
  const auto strcmp_entry=find("Ovb2dSJOAuE"),strncmp_entry=find("aesyjrHVWy4"),strcpy_entry=find("kiZSXIWd9vg"),strcpy_s_entry=find("5Xa2ACNECdo");
  const auto compare=[&](const char* a,const char* b) { return CallPortExport<int>(strcmp_entry,a,b); };
  const auto compare_n=[&](const char* a,const char* b,size_t n) { return CallPortExport<int>(strncmp_entry,a,b,n); };
  const auto copy=[&](char* a,const char* b) { return CallPortExport<char*>(strcpy_entry,a,b); };
  const auto copy_s=[&](char* a,size_t n,const char* b) { return CallPortExport<int>(strcpy_s_entry,a,n,b); };
  Check(compare("abc","abd")<0 && compare("abz","aba")>0 && compare("equal","equal")==0,"strcmp uses two-argument ABI");
  Check(compare_n("abc","abd",2)==0 && compare_n("abc","abd",3)<0 && compare_n("x","y",0)==0,"strncmp respects n");
  std::array<char,8> data{}; data.fill('!');
  Check(copy(data.data(),"abc")==data.data() && std::strcmp(data.data(),"abc")==0 && data[4]=='!',"strcpy uses its own NID");
  Check(copy_s(data.data(),4,"abc")==0 && data[4]=='!',"strcpy_s copies terminator and preserves guard");
  Check(copy_s(data.data(),3,"abc")==Libs::Posix::POSIX_ERANGE && data[0]==0 && data[4]=='!',"strcpy_s reports small destination");
  Check(copy_s(data.data(),data.size(),nullptr)==Libs::Posix::POSIX_EINVAL && data[0]==0,"strcpy_s null source");
  Check(copy_s(nullptr,8,"a")==Libs::Posix::POSIX_EINVAL && copy_s(data.data(),0,"a")==Libs::Posix::POSIX_EINVAL,"strcpy_s invalid destination");
  std::memcpy(data.data(),"abc",4);
  Check(copy_s(data.data(),8,data.data()+1)==Libs::Posix::POSIX_EINVAL && data[0]==0,"strcpy_s overlap is rejected");
}

void TestPm4FrontResourceState(Libs::Graphics::RenderContext& renderer) {
  using namespace Libs::Graphics;
  GraphicsInitJmpTables();
  CommandProcessor processor(renderer,0);
  constexpr uint32_t es=0x832c9125,ls=0x41ec528a;
  g_hw_sh_indirect_func[Pm4::SPI_SHADER_PGM_RSRC1_ES](processor,Pm4::SPI_SHADER_PGM_RSRC1_ES,es);
  std::array<uint32_t,2> payload{Pm4::SPI_SHADER_PGM_RSRC1_LS,ls};
  Check(CpOpSetShaderReg(processor,KYTY_PM4(3,Pm4::IT_SET_SH_REG,0),payload.data(),3,3)==2,"direct LS PM4 packet consumed");
  const auto& values=processor.GetShCtx().GetVs();
  Check(values.es_regs.rsrc1.programmed && values.es_regs.rsrc1.raw==es &&
        values.ls_regs.rsrc1.programmed && values.ls_regs.rsrc1.raw==ls,"ES/LS register writes retained independently");
  Check(values.es_regs.rsrc1.VgprBlocks()==0x25 && values.es_regs.rsrc1.SgprBlocks()==4 &&
        values.es_regs.rsrc1.FloatMode()==0xc9 && values.es_regs.rsrc1.VgprComponentCount()==3,"front register fields decoded");
  auto snapshot=processor.GetShCtx();
  processor.GetShCtx().Reset();
  Check(!processor.GetShCtx().GetVs().es_regs.rsrc1.programmed && !processor.GetShCtx().GetVs().ls_regs.rsrc1.programmed,"reset clears front resources");
  processor.GetShCtx()=snapshot;
  Check(processor.GetShCtx().GetVs().es_regs.rsrc1.raw==es && processor.GetShCtx().GetVs().ls_regs.rsrc1.raw==ls,"shader snapshot restores front resources");
}
