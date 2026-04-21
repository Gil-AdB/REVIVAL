/*

	 Object/Library reader, by Ofir Ehrlich
	 ======================================

	 Header file
	 Version: 1e-100000.0
	 TABs: 2

*/

enum Reader_Constants
{
// Module types
	Module_Undefined				 	 = 0,
	Module_Object 					 	 = 1,
	Module_Library	 				 	 = 2,
// Comment flags (in "Type" byte)
	Comment_NoPurgeBit			 	 = 1,
	Comment_NoListBit				 	 = 2,
// Comment classes
	Comment_ClassTranslator  	 = 0x00,
	Comment_ClassLibraryOld  	 = 0x81,
	Comment_ClassMemoryModel 	 = 0x9D,
	Comment_ClassDOSSEG			 	 = 0x9E,
	Comment_ClassLibraryNew  	 = 0x9F, // New version of 0x81 - Identical data
	Comment_ClassOMF				 	 = 0xA0,
	Comment_ClassNewOMF      	 = 0xA1,
	Comment_ClassLinkPass		 	 = 0xA2,
	Comment_ClassLibraryModule = 0xA3,
	Comment_ClassExecString  	 = 0xA4,
	Comment_ClassIncError			 = 0xA6,
	Comment_ClassNoSegPad			 = 0xA7,
	Comment_ClassWeakExtern	 	 = 0xA8,
	Comment_ClassLazyExtern	 	 = 0xA9,
	Comment_ClassPharLap		 	 = 0xAA,
	Comment_ClassRandom			 	 = 0xDA,
	Comment_ClassCompiler		 	 = 0xDB,
	Comment_ClassDate				 	 = 0xDC,
	Comment_ClassTimeStamp	 	 = 0xDD,
	Comment_ClassUser				 	 = 0xDF,
	Comment_ClassDependency	 	 = 0xE9,
	Comment_ClassCommandLine 	 = 0xFF;
}

//Structures
typedef struct
{
	unsigned char  Type;
	unsigned short Length;
} ChunkType;

typedef struct
{
	unsigned char Type;
	unsigned char Class;
	unsigned char Data;
} Comment;

typedef struct
{
	char *Name;
	unsigned char Type;
	Comment *CommentList;
}	Module;

// Local variables
FILE 				 *OBJFile;
Module 			 *CurMdl;
Comment 		 *Cmt;
unsigned char	Enable32BitValues;
unsigned char	CheckSum;
