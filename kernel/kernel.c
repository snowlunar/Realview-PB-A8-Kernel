#include "kernel.h"

#define PRINTPIDS 0

extern pcb_t *current;
extern hashtable_t* ht_ptr;
extern input_buff* ib_ptr;
extern pipes_t* pipes_ptr;
extern global_table* filetable_ptr;

//file_table_entry global_table[100];

entry_info_t programs[20];

uint32_t find_entry(char* pn){
    for(int i=0;i<20;i++){
        if(strcmp(pn,programs[i].name) == 0){
            return programs[i].entry;
        }
    }
    return -1;
}

void __user_exit(int r){
  asm volatile( "mov r0, %0 \n"
                "svc #3     \n"
              :
              : "r" (r)
              : "r0" );
}

//Transfers buffer control from process 'from' to process 'to'
//only if process 'from' has the control of the buffer
void __ioctl(pid_t from, pid_t to){
  if(from == to)
      return;
  if(ib_ptr->pid == from || ib_ptr->pid == 0){
    flush_buff(ib_ptr);
    ib_ptr->pid = to;
  }
}

int __close(pcb_t* p, int fd){
    if(!p->fdtable.fd[fd].active)
        return -1;
    int close_on_exit = p->fdtable.fd[fd].flags & 0xf00;
    int globalID = closeFileDes(p, fd);
    if(!filetable_ptr->entries[globalID].active)
        return -1;
    filetable_ptr->entries[globalID].refcount--;
    if(filetable_ptr->entries[globalID].refcount == 0 && close_on_exit == CLOSE_ON_EXIT){
        int inode = filetable_ptr->entries[globalID].inode;
        i_node parent = filetable_ptr->entries[globalID].parent_inode;
        int deleteRequested = filetable_ptr->entries[globalID].deleteRequested;
        file_type type = filetable_ptr->entries[globalID].type;
        switch(type){
            case stdio:
                close_global_entry(filetable_ptr, globalID);
                flush_buff(ib_ptr);
                break;
            case pipe:
                if(pipes_ptr->p[inode].len>0)
                    break;
                close_global_entry(filetable_ptr, globalID);
                close_pipe(pipes_ptr, inode);
                break;
            case file:
                close_global_entry(filetable_ptr, globalID);
                if(deleteRequested){
                    delete_dentry(parent, inode);
                }
                clear_file_cache(inode);
                break;
            default:
                break;
        }
    }
    return 0;
}

void __exit(ctx_t* ctx, int destroy, pid_t pid){
  pcb_t* temp = find_pid_ht(ht_ptr, pid);
  if(!temp->ph.parent)
      add_child(find_pid_ht(ht_ptr, 1), temp);
  if(pid == 1 || temp->ph.parent->pid < 1) //Do not exit the shell
    return;
  //deschedule(pid); finally here's the bug!!!
  pcb_t* parent = temp->ph.parent;
  if(parent->proc_state == WAITING){ //if parent is waiting, let it know that we are exiting
    if(destroy)
        parent->ctx.gpr[0] = ctx->gpr[0];
    else
        parent->ctx.gpr[0] = pid;
    unblock_by_pid(parent->pid);
  }
  __ioctl(pid, parent->pid);
  if(destroy){
    for(int i=0;i<MAXFD;i++){
        if(temp->fdtable.fd[i].active)
            __close(temp, i);
    }
    destroy_pid(pid);
  }
  else {
    setBlockInfo(temp, WAITING, -1, -1);
  }
  scheduler( ctx );
}

int __open(pcb_t* p, int fd, int globalID, file_type type, i_node inode, int flags){
    int i;
    if(!p)
        return -1;
    if(fd>=0 && p->fdtable.fd[fd].active)
        return -1;
    int fd2 = getFileDes(p, fd);//this is the return value
    if(fd2<0)
        return -1;
    switch(type){
        case stdio:
            if(!filetable_ptr->entries[0].active){
                get_global_entry(filetable_ptr, 0); //0 is always the stdio entry
                setGlobalEntry(filetable_ptr, 0, 0, type);
            }
            setFileDes(p, fd2, 0, flags, 0);
            filetable_ptr->entries[0].refcount++;
            break;
        //If given globalID is active, then choose that
        //Otherwise, check if given inode is active
        //Otherwise, generate or activate the given globalID and inode
        case pipe:
            if(globalID>=0 && filetable_ptr->entries[globalID].active){
                if(filetable_ptr->entries[globalID].type != type)
                    return -1;
                globalID = get_global_entry(filetable_ptr, globalID);
                inode = filetable_ptr->entries[globalID].inode;
                inode = get_pipe(pipes_ptr, inode);
            }
            else if(inode>=0 && pipes_ptr->p[inode].active) {
                globalID = pipes_ptr->p[inode].globalID;
                globalID = get_global_entry(filetable_ptr, globalID);
                inode = get_pipe(pipes_ptr, inode);
            }
            else {
                globalID = get_global_entry(filetable_ptr, globalID);
                inode = get_pipe(pipes_ptr, inode);
                setPipe(pipes_ptr, inode, globalID); //connect pipe with globalID
                setGlobalEntry(filetable_ptr, globalID, inode, type); //connect globalID with pipe
            }
            setFileDes(p, fd2, globalID, flags, 0); //connect FileDes with globalID
            filetable_ptr->entries[globalID].refcount++;
            break;
        case file:
            globalID = find_globalID_by_inode(filetable_ptr, inode, file);
            if(globalID == -1){
                globalID = get_global_entry(filetable_ptr, -1);
                setGlobalEntry(filetable_ptr, globalID, inode, type); //connect globalID with pipe
            }
            setFileDes(p, fd2, globalID, flags, 0); //connect FileDes with globalID
            filetable_ptr->entries[globalID].refcount++;
            break;
        default:
            return -1;
            break;
    }
    return fd2;
}

void process_char_stdout(uint8_t c){
  if( (c == BACKSPACE || c == DELETE) && !ib_ptr->ready && ib_ptr->n>0 ){
    PL011_putc(UART0, c);
    PL011_putc(UART0, ' ');
    PL011_putc(UART0, c);
  }
  else if(c == NEWLINE || c == RETURN){
    PL011_putc(UART0, '\n');
  }
  else if(c>=0x20 && c<=0x7E && !ib_ptr->ready){
    PL011_putc(UART0, c);
  }
}

void kernel_handler_rst( ctx_t* ctx              ) {
  /* Configure the mechanism for interrupt handling by
   *
   * - configuring timer st. it raises a (periodic) interrupt for each 
   *   timer tick,
   * - configuring GIC st. the selected interrupts are forwarded to the 
   *   processor via the IRQ interrupt signal, then
   * - enabling IRQ interrupts.
   */

  UART0->IMSC           |= 0x00000010; // enable UART    (Rx) interrupt
  UART0->CR              = 0x00000301; // enable UART (Tx+Rx)

  TIMER0->Timer1Load     = 0x00010000; // select period = 2^18 ticks ~= 0.25 sec
  TIMER0->Timer1Ctrl     = 0x00000002; // select 32-bit   timer
  TIMER0->Timer1Ctrl    |= 0x00000040; // select periodic timer
  TIMER0->Timer1Ctrl    |= 0x00000020; // enable          timer interrupt
  TIMER0->Timer1Ctrl    |= 0x00000080; // enable          timer

  GICC0->PMR             = 0x000000F0; // unmask all            interrupts
  GICD0->ISENABLER[ 1 ] |= 0x00000010; // enable timer          interrupt
  GICD0->ISENABLER[ 1 ] |= 0x00001000; // enable UART    (Rx) interrupt
  GICC0->CTLR            = 0x00000001; // enable GIC interface
  GICD0->CTLR            = 0x00000001; // enable GIC distributor

  /* Initialise PCBs representing processes stemming from execution of
   * the two user programs.  Note in each case that
   *    
   * - the CPSR value of 0x50 means the processor is switched into USR 
   *   mode, with IRQ interrupts enabled, and
   * - the PC and SP values match the entry point and top of stack. 
   */
    
  programs[0].name  = "P0";
  programs[0].entry = (uint32_t)entry_P0;
  programs[1].name  = "P1";
  programs[1].entry = (uint32_t)entry_P1;
  programs[2].name  = "P2";
  programs[2].entry = (uint32_t)entry_P2;
  programs[3].name  = "genPrimes";
  programs[3].entry = (uint32_t)entry_genPrimes;
  programs[4].name  = "testPipe";
  programs[4].entry = (uint32_t)entry_testPipe;
  programs[5].name  = "P3";
  programs[5].entry = (uint32_t)entry_P3;
  programs[6].name  = "shell";
  programs[6].entry = (uint32_t)entry_shell;
  programs[7].name  = "Dining";
  programs[7].entry = (uint32_t)entry_Dining;
  programs[8].name  = "grep";
  programs[8].entry = (uint32_t)entry_grep;
  programs[9].name  = "edit";
  programs[9].entry = (uint32_t)entry_edit;
  

  initialise_scheduler((uint32_t)&tos_irq);
  
  pid_t temp = init_pcb((uint32_t)entry_shell);
  pcb_t* p = find_pid_ht(ht_ptr, temp);
  __open(p, 0, -1, stdio, -1, READ_ONLY|KEEP_ON_EXEC);
  __open(p, 1, -1, stdio, -1, WRITE_ONLY|KEEP_ON_EXEC);
  p->cwd_inode = 0;
  schedule(temp);

  if(load_sb())
    format_disk();

  /* Once the PCBs are initialised, we (arbitrarily) select one to be
   * restored (i.e., executed) when the function then returns.
   */

  scheduler(ctx);

  irq_enable();

  return;
}

void kernel_handler_irq( ctx_t* ctx ) {
  // Step 2: read the interrupt identifier so we know the source.

  uint32_t id = GICC0->IAR;

  // Step 4: handle the interrupt, then clear (or reset) the source.
  switch(id){
    case GIC_SOURCE_TIMER0 : {
      scheduler( ctx );
      TIMER0->Timer1IntClr = 0x01;
      break;
    }
    case GIC_SOURCE_UART0 : {
      uint8_t c = PL011_getc( UART0 );
      if(c == CTRLC){ //exit the foreground process. have an internal _exit() call
        __exit(ctx, 1, ib_ptr->pid);
      }
      else if(c == CTRLZ){
        __exit(ctx, 0, ib_ptr->pid);
      }
      else{
        process_char_stdout(c);
      }
      int ready = process_char(ib_ptr,c);
      if(ready){
        unblock_by_pid(ib_ptr->pid);
      }
      UART0->ICR = 0x10;
      break;
    }
    default : {
      break;
    }
  }

  // Step 5: write the interrupt identifier to signal we're done.
  GICC0->EOIR = id;

}

void kernel_handler_svc( ctx_t* ctx, uint32_t id ) { 
  /* Based on the identified encoded as an immediate operand in the
   * instruction, 
   *
   * - read  the arguments from preserved usr mode registers,
   * - perform whatever is appropriate for this system call,
   * - write any return value back to preserved usr mode registers.
   */

  switch( id ) {
    case 0x00 : { // yield()
      scheduler( ctx );
      break;
    }
    case 0x01 : { // write( fd, x, n )
      int   fd = ( int   )( ctx->gpr[ 0 ] );
      fd_entry* fentry = &current->fdtable.fd[fd];
      if(!fentry->active || (fentry->flags & 0xf) == READ_ONLY){
        ctx->gpr[3] = -1;
        break;
      }
      global_entry* gentry = &filetable_ptr->entries[fentry->globalID];
      if(!gentry->active){
        ctx->gpr[3] = -1;
        break;
      }
      switch(gentry->type){
        case stdio : {
          char*  x = ( char* )( ctx->gpr[ 1 ] );  
          int    n = ( int   )( ctx->gpr[ 2 ] );
          pid_t pid = current->pid;
          if(pid>1 && PRINTPIDS){
            PL011_puth(UART0, pid);
            PL011_putc(UART0, ':');
            PL011_putc(UART0, ' ');
          }
          for( int i = 0; i < n; i++ ) {
            PL011_putc( UART0, *x++ );
          }
          ctx->gpr[ 3 ] = n;
          break;
        }
        case pipe : {
          char* x = (char*)(ctx->gpr[1]);
          int  n = (int )(ctx->gpr[2]);
          pipe_t* pentry = &pipes_ptr->p[gentry->inode];
          if(!pentry->active){
            ctx->gpr[3] = -1;
            break;
          }
          int i;
          for(i=0;i<n;i++){
            if(isPipeFull(pipes_ptr, pentry->inode)){
              ctx->pc = ctx->pc - 0x4; //correct address so next time the process unblocks, it will try again
              ctx->gpr[1] = ctx->gpr[1] + i*sizeof(char);
              ctx->gpr[2] = ctx->gpr[2] - i;
              pid_t unblock_next = dequeue_wq(filetable_ptr, gentry->globalID, 1);
              setBlockInfo(current, BLOCKED, fd, 0);
              enqueue_wq(filetable_ptr, gentry->globalID, 0, current->pid);
              if(unblock_next>0 && unblock_next!=current->pid)
                unblock_by_pid(unblock_next);
              scheduler(ctx);
              break;
            }
            else {
              send_pipe(pipes_ptr, pentry->inode, x[i]);
              ctx->gpr[3] = ctx->gpr[3] + 1;
            }
          }
          if(i!=n) //if for loop was broken
            break;
          pid_t unblock_next = dequeue_wq(filetable_ptr, gentry->globalID, 1); //is anyone available to read what I've just written?
          if(unblock_next<=0)
            unblock_next = dequeue_wq(filetable_ptr, gentry->globalID, 0); //if not, is anyone available to write?
          if(unblock_next>0 && unblock_next!=current->pid)
            unblock_by_pid(unblock_next);  
          break;
        }
        case file : {
          char* x = (char*)(ctx->gpr[1]);
          int  n = (int )(ctx->gpr[2]);
          for(int i=0;i<n;i++){
            write_file(gentry->inode,fentry->rwpointer++,x[i]);
          }
          ctx->gpr[3] = n;
          break;
        }
        default : {
          ctx->gpr[3] = -1;
          break;
        }
      }
      break;
    }
    case 0x02 : { //fork()
      update_ctx(current, ctx);
      pid_t child = init_pcb_by_pointer(current); //check for errors here
      ctx->gpr[0] = child;
      pcb_t* childPCB = find_pid_ht(ht_ptr, child);
      childPCB->ctx.gpr[0] = 0;
      for(int i=0;i<MAXFD;i++){
        if(childPCB->fdtable.fd[i].active){
            int globalID = childPCB->fdtable.fd[i].globalID;
            filetable_ptr->entries[globalID].refcount++;
        }
      }
      schedule(child);
      break;
    }
    case 0x03 : { //exit()
      __exit(ctx, 1, current->pid);
      break;
    }
    case 0x04 : { //read(fd, x, n)
      int fd = (int)(ctx->gpr[0]);
      fd_entry* fentry = &current->fdtable.fd[fd];
      if(!fentry->active || (fentry->flags & 0xf) == WRITE_ONLY){
        ctx->gpr[3] = -1;
        break;
      }
      global_entry* gentry = &filetable_ptr->entries[fentry->globalID];
      if(!gentry->active){
        ctx->gpr[3] = -1;
        break;
      }
      switch(gentry->type){
        case stdio : {
          //when you press enter, the buffer should become ready
          //it should stay ready until i == n
          //do not accept characters while it's ready
          //once flushed accept more
          char*  x = ( char* )( ctx->gpr[ 1 ] );
          int    n = ( int   )( ctx->gpr[ 2 ] );
          if(ib_ptr->pid != current->pid || !ib_ptr->ready){
            __ioctl(0, current->pid); //if nobody has ownership then give ownership
            ctx->pc = ctx->pc - 0x4; //correct address so next time the process unblocks, it will try again
            setBlockInfo(current, BLOCKED, fd, -1);
            scheduler(ctx);
            break;
          }
          int i = 0;
          for(i=0;i<n;i++){
            x[i] = consume_char(ib_ptr);
            if(!ib_ptr->ready){
              i = i + 1;
              break;
            }
          }
          ctx->gpr[3] = i;
          break;
        }
        case pipe : {
          //reading from pipe should block until
          //all n bytes are received
          char* x = (char*)(ctx->gpr[1]);
          int  n = (int )(ctx->gpr[2]);
          pipe_t* pentry = &pipes_ptr->p[gentry->inode];
          if(!pentry->active){
            ctx->gpr[3] = -1;
            break;
          }
          int i;
          for(i=0;i<n;i++){
            if(isPipeEmpty(pipes_ptr, pentry->inode)){
              ctx->pc = ctx->pc - 0x4; //correct address so next time the process unblocks, it will try again
              ctx->gpr[1] = ctx->gpr[1] + i*sizeof(char);
              ctx->gpr[2] = ctx->gpr[2] - i;
              pid_t unblock_next = dequeue_wq(filetable_ptr, gentry->globalID, 0);
              setBlockInfo(current, BLOCKED, fd, 1);
              enqueue_wq(filetable_ptr, gentry->globalID, 1, current->pid);
              if(unblock_next>0 && unblock_next!=current->pid)
                unblock_by_pid(unblock_next);
              scheduler(ctx);
              break;
            }
            else {
              x[i] = consume_pipe(pipes_ptr, pentry->inode);
              ctx->gpr[3] = ctx->gpr[3] + 1;
            }
          }
          if(i!=n) //If for loop was broken
            break;
          pid_t unblock_next = dequeue_wq(filetable_ptr, gentry->globalID, 1); //is anyone available to read some more?
          if(unblock_next<=0)
            unblock_next = dequeue_wq(filetable_ptr, gentry->globalID, 0); //if not, is anyone available to write?
          if(unblock_next>0 && unblock_next!=current->pid)
            unblock_by_pid(unblock_next);         
          break;
        }
        case file : {
          char* x = (char*)(ctx->gpr[1]);
          int  n = (int )(ctx->gpr[2]);
          for(int i=0;i<n;i++){
            x[i] = read_file(gentry->inode,fentry->rwpointer++);
          }
          ctx->gpr[3] = n;
          break;
        }
        default : {
          ctx->gpr[3] = -1;
          break;
        }
      }
      break;
    }
    case 0x05 : { //waitpid(int pid) if pid is not in the queue, add it.
      int pid = (int)(ctx->gpr[0]);
      pcb_t* temp = find_pid_ht(ht_ptr,pid);
      if(temp==NULL)
        break;
      //if(temp->proc_state == BLOCKED)
      //  break;
      __ioctl(current->pid,pid);
      proc_state_t reason;
      if(current->pid == pid) //waiting for self. that means block
        reason = BLOCKED;
      else { //make this work only if pid is a child of current
        reason = WAITING;
        unblock_by_pid(pid);//if it is blocked
      }
      setBlockInfo(current, reason, 0, 0);
      scheduler( ctx );
      break;
    }
    case 0x06 : { //exec("program name",[*a0,*a1,*a2,...])
      char*  pn   = (char * ) ctx->gpr[0];
      char** argv = (char **) ctx->gpr[1];
      int i = 0;

      uint32_t pc = find_entry(pn);
      if(pc==-1){
          ctx->gpr[0] = -2;
          break;
      }

      char* arg_addr[128];

      ctx->pc  = pc;
      char* sp = (char*)(long)current->stack.tos;

      while(argv[i]){
        int len = strlen(argv[i]) + 1;
        sp = sp - len;
        memcpy(sp, argv[i], len);
        arg_addr[i++] = sp;
      }
      arg_addr[i] = 0;

      //align %8
      sp = sp - 4;
      while((uint32_t)sp%8) sp = sp - 1;
      
      sp = sp - (i+1)*sizeof(char*);
      memcpy(sp, arg_addr, (i+1)*sizeof(char*));
      
      ctx->pc = pc;
      ctx->sp = (uint32_t)sp;
      ctx->lr = (uint32_t)&__user_exit;
      ctx->gpr[0] = i;
      ctx->gpr[1] = ctx->sp;
      for(int i=0;i<MAXFD;i++){
        if(current->fdtable.fd[i].active){
            if((current->fdtable.fd[i].flags & 0xf0) == CLOSE_ON_EXEC){
                __close(current, i);
            }
        }
      }
      update_ctx(current, ctx);
      break;
    }
    case 0x07 : { //int pids=procs(int* buff)
      int* buff = (int*) ctx->gpr[0];
      pcb_t* temp = ht_ptr->keyset.head;
      int i = 0;
      while(temp){
        buff[i++] = temp->pid;
        temp = temp->next;
      }
      ctx->gpr[0] = i;
      break;
    }
    case 0x08 : { //char* procstat(int pid)
      pid_t pid = (pid_t) ctx->gpr[0];
      pcb_t* pcb = find_pid_ht(ht_ptr, pid);
      if(!pcb)
        ctx->gpr[0] = -1; //terminated
      else if(pid == ib_ptr->pid)
        ctx->gpr[0] = 0; //foreground
      else if(pcb->proc_state == RUNNING || pcb->proc_state == READY)
        ctx->gpr[0] = 1; //running
      else if(pcb->proc_state == BLOCKED)
        ctx->gpr[0] = 2; //blocked
      else
        ctx->gpr[0] = 3; //stopped temporarily
      break;
    }
    case 0x09 : { //kill(pid, signal)
      int pid    = (int) ctx->gpr[0];
      int signal = (int) ctx->gpr[1];
      switch(signal){
        case SIGCONT:{
            pcb_t* temp = find_pid_ht(ht_ptr,pid);
            if(temp==NULL)
                break;
            //if(temp->proc_state == BLOCKED)
            //    break;
            unblock_by_pid(pid);//if it is blocked
            //scheduler( ctx );
            break;
        }
        case SIGTERM:{
            __exit(ctx, 1, pid);
            break;
        }
        default:
            break;
      }
      break;
    }
    case 0x0A : { //nice(int pid, int priority) should only be called by shell
      int pid      = (int) ctx->gpr[0];
      int priority = (int) ctx->gpr[1];
      if(current->pid != 1)
        break;
      pcb_t* p = find_pid_ht(ht_ptr,pid);
      if(!p)
        break;
      setPriority(p, priority);
      break;
    }
    case 0x0B : { //int getpipe()
      int fd =  __open(current, -1, -1, pipe, -1, READ_WRITE|KEEP_ON_EXEC);
      ctx->gpr[0] = fd;
      break;
    }
    case 0x0C : { //int fcntl(fd, flags)
      int fd    = (int) ctx->gpr[0];
      int flags = (int) ctx->gpr[1];
      if(!current->fdtable.fd[fd].active){
          ctx->gpr[0] = -1;
          break;
      }
      current->fdtable.fd[fd].flags = flags;
      ctx->gpr[0] = 0;
      break;
    }
    case 0x0D : { //int redir(from, to) "to" must be open. original from will close
      int from = (int) ctx->gpr[0];
      int to   = (int) ctx->gpr[1];
      if(!current->fdtable.fd[to].active){
          ctx->gpr[0] = -1;
          break;
      }
      __close(current, from);
      int globalID   = current->fdtable.fd[to].globalID;
      file_type type = filetable_ptr->entries[globalID].type;
      __open(current, from, globalID, type, -1, READ_WRITE|KEEP_ON_EXEC);
      ctx->gpr[0] = 0;
      break;
    }
    case 0x0E : { //close(fd)
      int fd = (int) ctx->gpr[0];
      __close(current, fd);
      break;
    }
    case 0x0F : { //int getppid(pid)
      int pid = (int) ctx->gpr[0];
      pcb_t* p = find_pid_ht(ht_ptr,pid);
      if(!p){
        ctx->gpr[0] = -1;
        break;
      }
      pcb_t* pp = p->ph.parent;
      int ppid;
      if(!pp)
        ppid = 1;
      else
        ppid = pp->pid;
      ctx->gpr[0] = ppid;
      break;
    }
    case 0x10 : { //creat(path) creates file(not a directory)
      char*  path0   = (char * ) ctx->gpr[0];
      char path[100];
      path[0]='\0';
      int start = 0;
      if(path0[0]!='/'){
        start = current->cwd_inode;
        strcat(path, "./"); 
      }
      strcat(path, path0);
      char*  lastslash = path;
      int i = 0;
      for(i=0;path[i]!='\0';i++){
        if(path[i]=='/'){
            lastslash = &path[i];
        }
      }
      *lastslash = '\0';
      i_node parent_inode = parse_path(path, start);
      if(parent_inode == -1){
        ctx->gpr[0] = -1;
      }
      else{
        ctx->gpr[0] = create_dentry(parent_inode, ++lastslash, 0, -1);
      }
      break;
    }
    case 0x11 : { //int fd = open(path,flags)
      char* path0 = (char*) ctx->gpr[0];
      char path[100];
      path[0]='\0';
      int start = 0;
      if(path0[0]!='/'){
        start = current->cwd_inode;
        strcat(path, "./");
      }
      strcat(path, path0);
      int  flags = (int  ) ctx->gpr[1];

      i_node inode = parse_path(path, start);
      if(inode == -1){
          ctx->gpr[0] = -1;
          break;
      }
      uint32_t used = read_inode_field(inode, 0);
      if(!used){
          ctx->gpr[0] = -1;
          break;
      }
      uint32_t mode = read_inode_field(inode, 1);
      if(mode == 0){
          ctx->gpr[0] = -1;
          break;
      }
      if((0xf000&flags) == CLEAR_FILE){
          clear_file(inode);
      }
      ctx->gpr[0] = __open(current, -1, -1, file, inode, flags);
      break;
    }
    case 0x12 : { //unlink(char* path)
      char*  path0   = (char * ) ctx->gpr[0];
      char path[100];
      path[0]='\0';
      int start = 0;
      if(path0[0]!='/'){
        start = current->cwd_inode;
        strcat(path, "./"); 
      }
      strcat(path, path0);
      char* lastslash = path;

      int i = 0;
      for(i=0;path[i]!='\0';i++){
          if(path[i]=='/'){
              lastslash = &path[i];
          }
      }
      *lastslash = '\0';
      i_node parent_inode = parse_path(path, start);
      if(parent_inode == -1){
          ctx->gpr[0] = -1;
          break;
      }

      i_node inode = find_file(parent_inode, ++lastslash);
      uint32_t mode = read_inode_field(inode, 1);
      if(mode == 0){
          ctx->gpr[0] = -1;
          break;
      }
      int globalID = find_globalID_by_inode(filetable_ptr, inode, file);
      if(globalID == -1){
          delete_dentry(parent_inode, inode);
      }
      else{
          filetable_ptr->entries[globalID].deleteRequested = 1;
          filetable_ptr->entries[globalID].parent_inode = parent_inode;
      }
      ctx->gpr[0] = 0;

      break;
    }
    case 0x13 : { //int lseek(fd, offset, whence)
        int fd     = (int) ctx->gpr[0];
        int offset = (int) ctx->gpr[1];
        int whence = (int) ctx->gpr[2];
        fd_entry* fentry = &current->fdtable.fd[fd];
        if(!fentry->active){
            ctx->gpr[0] = -1;
            break;
        }
        global_entry* gentry = &filetable_ptr->entries[fentry->globalID];
        if(!gentry->active){
            ctx->gpr[0] = -1;
            break;
        }
        if(whence == SEEK_SET){
            fentry->rwpointer = offset;
        }
        else if(whence == SEEK_CUR){
            fentry->rwpointer = fentry->rwpointer + offset;
        }
        else if(whence == SEEK_END){
            uint32_t size = read_inode_field(gentry->inode, 2);
            fentry->rwpointer = size + offset;
        }
        else{
            ctx->gpr[0] = -1;
            break;
        }
        ctx->gpr[0] = fentry->rwpointer;
        break;
    }
    case 0x14 : { //mkdir(char* path)
      char*  path0   = (char * ) ctx->gpr[0];
      char path[100];
      path[0]='\0';
      int start = 0;
      if(path0[0]!='/'){
        start = current->cwd_inode;
        strcat(path, "./"); 
      }
      strcat(path, path0);
      char*  lastslash = path;

      int i = 0;
      for(i=0;path[i]!='\0';i++){
          if(path[i]=='/'){
              lastslash = &path[i];
          }
      }
      *lastslash = '\0';
      i_node parent_inode = parse_path(path, start);
      if(parent_inode == -1){
          ctx->gpr[0] = -1;
      }
      else{
          ctx->gpr[0] = create_dentry(parent_inode, ++lastslash, 1, -1);
      }

      break;
    }
    case 0x15 : { //rmdir(char* path)
      char*  path0   = (char * ) ctx->gpr[0];
      char path[100];
      path[0]='\0';
      int start = 0;
      if(path0[0]!='/'){
        start = current->cwd_inode;
        strcat(path, "./"); 
      }
      strcat(path, path0);
      char* lastslash = path;

      int i = 0;
      for(i=0;path[i]!='\0';i++){
          if(path[i]=='/'){
              lastslash = &path[i];
          }
      }
      *lastslash = '\0';
      i_node parent_inode = parse_path(path, start);
      if(parent_inode == -1){
          ctx->gpr[0] = -1;
          break;
      }

      i_node inode = find_file(parent_inode, ++lastslash);
      uint32_t mode = read_inode_field(inode, 1);
      if(mode == 1){
          ctx->gpr[0] = -1;
          break;
      }
      ctx->gpr[0] = delete_dentry(parent_inode, inode);

      break;
    }
    case 0x16 : { //int getcwd(char* buff) PROBABLY SLOW. IMPROVE THIS
      char* buff = (char*) ctx->gpr[0];
      buff[0] = '\0';
      i_node curr = current->cwd_inode;
      char temp[12];
      temp[0] = '\0';
      char temp2[100];
      temp2[0] = '\0';
      int res = 0;
      while(1){
        i_node up = find_file(curr,"..");
        if(up == curr){
            break;
        }
        res = find_file_by_inode(up, curr, temp);
        if(res==-1)
            break;
        curr = up;
        temp2[0] = '\0';
        strcat(temp2, buff);
        buff[0] = '/';
        buff[1] = '\0';
        strcat(buff, temp);
        temp[0]='\0';
        strcat(buff, temp2);
      }
      if(res==-1){
        ctx->gpr[0] = -1;
        break;
      }
      if(buff[0]!='/'){
        temp2[0]='\0';
        strcat(temp2,buff);
        buff[0]='/';
        buff[1]='\0';
        strcat(buff, temp2);
      }
      ctx->gpr[0] = 0;
      break;
    }
    case 0x17 : { //int getdents(char* buff)
      char* buff = (char*) ctx->gpr[0];
      buff[0] = '\0';
      dentry_t dentries[20];
      uint8_t* ptr = (uint8_t*)dentries;
      uint32_t parsize = read_inode_field(current->cwd_inode, 2);
      for(int c=0;c<parsize;c++){
        ptr[c] = read_file(current->cwd_inode,c);
      }
      for(int c=0;c<parsize/sizeof(dentry_t);c++){
        strcat(buff,dentries[c].name);
        strcat(buff,"|");
      }
      ctx->gpr[0] = parsize/sizeof(dentry_t);
      break;
    }
    case 0x18 : { //int chdir(char* path)
      char*  path0   = (char * ) ctx->gpr[0];
      char path[100];
      path[0]='\0';
      int start = 0;
      if(path0[0]!='/'){
        start = current->cwd_inode;
        strcat(path, "./"); 
      }
      strcat(path, path0);

      i_node inode = parse_path(path, start);
      if(inode == -1){
          ctx->gpr[0] = -1;
          break;
      }
      uint32_t mode = read_inode_field(inode, 1);
      if(mode == 1){
           ctx->gpr[0] = -1;
           break;
      }
      current->cwd_inode = inode;
      ctx->gpr[0] = 0;

      break;
    }
    case 0x19 : { //sync(fd) should only be used when a pipe is used by 2 processes ONLY
      int fd = (int)(ctx->gpr[0]);
      fd_entry* fentry = &current->fdtable.fd[fd];
      if(!fentry->active){
        break;
      }
      global_entry* gentry = &filetable_ptr->entries[fentry->globalID];
      if(!gentry->active){
        break;
      }
      switch(gentry->type){
        case stdio : {
          break;
        }
        case pipe : {
          pipe_t* pentry = &pipes_ptr->p[gentry->inode];
          if(!pentry->active){
            break;
          }
          insertSyncPid(pipes_ptr, gentry->inode, current->pid);
          if(pentry->sync.flag2 == 0){
            setBlockInfo(current, BLOCKED, -1, -1);
            ctx->pc = ctx->pc - 0x4;
            pentry->sync.flag = 1;
            pid_t other = getOtherSyncPid(pipes_ptr, gentry->inode, current->pid);
            if(other>0){
                unblock_by_pid(other);
            }
            scheduler(ctx);
          }else{
            pentry->sync.flag2 = 0;
            pid_t other = getOtherSyncPid(pipes_ptr, gentry->inode, current->pid);
            if(other>0){
                unblock_by_pid(other);
            }
          }
          break; 
        } 
        case file : {
          break;
        }
      }
      break;
    }
    case 0x1A : { //int fd = select(fd[n], int n)
      int* fds = (int*) ctx->gpr[0];
      int    n = (int ) ctx->gpr[1];
      int fd_ready = -1;
      int index = 0;
      for(int i = 0;i<n && fd_ready==-1;i++){
          fd_entry* fentry = &current->fdtable.fd[fds[i]];
          if(!fentry->active){
            continue;
          }
          global_entry* gentry = &filetable_ptr->entries[fentry->globalID];
          if(!gentry->active){
            continue;
          }
          switch(gentry->type){
            case stdio:
                break;
            case pipe:{
                pipe_t* pentry = &pipes_ptr->p[gentry->inode];
                if(!pentry->active){
                    break;
                }
                insertSyncPid(pipes_ptr, gentry->inode, current->pid);
                if(pentry->sync.flag == 1){
                    fd_ready = fds[i];
                    index = i;
                    pentry->sync.flag  = 0;//maybe reset only when empty?
                    pentry->sync.flag2 = 1;
                    pid_t other = getOtherSyncPid(pipes_ptr, gentry->inode, current->pid);
                    if(other>0){
                        unblock_by_pid(other);
                    }
                }
                break;
            }
            case file:
                break;
          }
      }
      if(fd_ready>=0){
        ctx->gpr[0] = index;
      }
      else{
        setBlockInfo(current, BLOCKED, -1, -1);
        ctx->pc = ctx->pc - 0x4;
        scheduler(ctx);
      }
      break;
    }
    case 0x1B : { //simkey(char c)
      char c = (char) ctx->gpr[0];
      process_char_stdout(c);
      process_char(ib_ptr,c);
      break;
    }
    case 0x1C : { //clear_cache()
      empty_cache();
      break;
    }
    default   : { // unknown
      break;
    }
  }

  return;
}


