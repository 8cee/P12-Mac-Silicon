#pragma once
namespace p12 {
// Called only while a newly loaded processor is private to the control thread.
// Clear every block: an effect's output must not become its next warm-up input.
template<class Clear, class Process>
void warmPlugin(int blocks, Clear clear, Process process) {
    for(int i=0;i<blocks;++i){clear();process();}
}
}
