`timescale 1ns / 1ns
//////////////////////////////////////////////////////////////////////////////////
// Company: Tiray
// Engineer: capybara
// Create Date: 2026.6.22
// Design Name:
// Module Name: 
// Project Name:
// Target Devices:
// Tool Versions:
// Description:
//
// Dependencies:
//
// Revision:
// Revision 0.01 - file created @ 2026.6.22.
// Additional Comments : revision 0.01 : 1, pu configure pa params through upm.
//
//////////////////////////////////////////////////////////////////////////////////

module pa_pu_com
#(
  parameter WID_8                           = 8    ,
  parameter WID_16                          = 16   ,
  parameter WID_32                          = 32   ,
  parameter PA_VERSION                      = 32'd0,
  parameter PA_BUILD_INFO                   = 32'd0,
  parameter ADAPTED_MAIN_BOAD_VERSION       = 32'd0,   
  parameter ADAPTED_GIC_BOAD_VERSION        = 32'd0,      
  parameter ADAPTED_ROIC_BOAD_VERSION       = 32'd0,    
  parameter ADAPTED_RESERVED_BOAD_0_VERSION = 32'd0,
  parameter ADAPTED_RESERVED_BOAD_1_VERSION = 32'd0,
  parameter ADAPTED_RESERVED_BOAD_2_VERSION = 32'd0,
  parameter DBG_EN                          = 0    
)
(
  //-- clk & rst.
  input                   clk                 ,
  input                   rst_n               ,
                                                                
  //-- pu upm configure ports.  
  input      [31:0]       upm_pu_awaddr       ,
  input                   upm_pu_awlock       ,
  input                   upm_pu_awvalid      ,
  input      [11:0]       upm_pu_awid         ,
  output reg              upm_pu_awready = 'd0,
  input      [63:0]       upm_pu_wdata        ,
  output reg              upm_pu_wready  = 'd0,
  input                   upm_pu_wvalid       ,
  input                   upm_pu_bready       ,
  output reg [11:0]       upm_pu_bid     = 'd0,
  output reg [1:0]        upm_pu_bresp   = 'd0,
  output reg              upm_pu_bvalid  = 'd0,
  input      [31:0]       upm_pu_araddr       ,
  input      [11:0]       upm_pu_arid         ,
  input                   upm_pu_arvalid      ,
  output reg              upm_pu_arready = 'd0,
  output reg              upm_pu_rlast   = 'd0,
  output reg              upm_pu_rvalid  = 'd0,
  input                   upm_pu_rready       ,
  output     [63:0]       upm_pu_rdata        ,
  output reg [11:0]       upm_pu_rid     = 'd0,
  output reg [1:0]        upm_pu_rresp   = 'd0,
  
  output reg              irq_pa_2_pu    = 'd0,
  
  //-- interface with sub module.
  //-- gic module.
  input                   gic_rst_init_state       ,
  output reg              gic_str             = 'd0,
  output reg              gic_stop            = 'd0,
  output reg [WID_8-1:0]  gic_req_code        = 'd0,
  output reg              gic_dout_en         = 'd0,
  output reg [WID_32-1:0] gic_line_time       = 'd0,
  output reg [WID_32-1:0] gic_oe_raising_edge = 'd0,
  output reg [WID_32-1:0] gic_oe_falling_edge = 'd0,
  output reg [WID_16-1:0] gic_str_row_num     = 'd0,
  output reg [WID_16-1:0] gic_end_row_num     = 'd0,
  output reg [WID_8-1:0]  gic_binning_mode    = 'd0,
  input                   gic_state                ,
  input                   gic_end                  ,
  input      [WID_8-1:0]  gic_dfx                  ,
  output reg [WID_32-1:0] gic_debug_in        = 'd0,
  input      [WID_32-1:0] gic_debug_out            ,
  
  //-- roic module                       
  input                    roic_rst_init_state        ,
  output reg               roic_str              = 'd0,
  output reg  [WID_8-1:0]  roic_req_code         = 'd0,
  output reg  [WID_16-1:0] roic_reg_00           = 'd0,
  output reg  [WID_16-1:0] roic_reg_02           = 'd0,
  output reg  [WID_16-1:0] roic_reg_05           = 'd0,
  output reg  [WID_16-1:0] roic_reg_06           = 'd0,
  output reg  [WID_16-1:0] roic_reg_07           = 'd0,
  output reg  [WID_16-1:0] roic_reg_09           = 'd0,
  output reg  [WID_16-1:0] roic_reg_0a           = 'd0,
  output reg  [WID_16-1:0] roic_reg_0b           = 'd0,
  output reg  [WID_16-1:0] roic_reg_0c           = 'd0,
  output reg  [WID_16-1:0] roic_reg_0d           = 'd0,
  output reg  [WID_16-1:0] roic_reg_0e           = 'd0,
  output reg  [WID_16-1:0] roic_reg_0f           = 'd0,
  output reg  [WID_16-1:0] roic_reg_10           = 'd0,
  output reg  [WID_16-1:0] roic_reg_11           = 'd0,
  output reg  [WID_16-1:0] roic_reg_17           = 'd0,
  output reg  [WID_16-1:0] roic_reg_24           = 'd0,
  output reg  [WID_16-1:0] roic_reg_28           = 'd0,
  output reg  [WID_16-1:0] roic_reg_2d           = 'd0,
  output reg  [WID_16-1:0] roic_reg_3b           = 'd0,
  output reg  [WID_16-1:0] roic_str_col_num      = 'd0,
  output reg  [WID_16-1:0] roic_end_col_num      = 'd0,
  output reg  [WID_8-1:0]  roic_binning_mode     = 'd0,
  input                    roic_state                 ,
  input                    roic_end                   ,
  input       [WID_8-1:0]  roic_dfx                   ,
  output reg  [WID_32-1:0] roic_debug_in         = 'd0,
  input       [WID_32-1:0] roic_debug_out             ,
  
  //-- img_wr module.
  input                   img_wr_rst_init_state      ,
  output reg              img_wr_str            = 'd0,
  output reg [WID_32-1:0] img_wr_str_addr       = 'd0,
  input                   img_wr_state               ,
  input                   img_wr_end                 ,
  input      [WID_8-1:0]  img_wr_dfx                 ,
  output reg [WID_32-1:0] img_wr_debug_in       = 'd0,
  input      [WID_32-1:0] img_wr_debug_out           ,
  
  //-- img_corr module.
  input                   img_corr_rst_init_state            ,
  output reg              img_corr_str                  = 'd0,
  output reg [WID_16-1:0] img_pkg_num                   = 'd0,
  output reg [WID_16-1:0] img_row_num                   = 'd0,
  output reg [WID_16-1:0] img_col_num                   = 'd0,
  output reg              img_corr_offset_en            = 'd0,
  output reg [WID_32-1:0] img_corr_offset_temp_str_addr = 'd0,
  output reg [WID_16-1:0] img_corr_offset_adder_value   = 'd0,
  output reg              img_corr_gain_en              = 'd0,
  output reg [WID_32-1:0] img_corr_gain_temp_str_addr   = 'd0,
  output reg [WID_16-1:0] img_corr_gain_clipping_value  = 'd0,
  output reg              img_corr_defect_en            = 'd0,
  input                   img_corr_state                     ,
  input                   img_corr_end                       ,
  input      [WID_8-1:0]  img_corr_dfx                       ,
  output reg [WID_32-1:0] img_corr_debug_in             = 'd0,
  input      [WID_32-1:0] img_corr_debug_out                 
);

  localparam PA_PU_COM_VERSION = 32'h00_00_00_00;
  
  reg              wr_en   = 'd0;
  reg [WID_16-1:0] wr_addr = 'd0;
  reg [WID_32-1:0] wr_data = 'd0;
  reg              rd_en   = 'd0;
  reg [WID_16-1:0] rd_addr = 'd0;
  reg [WID_32-1:0] rd_data = 'd0;

  always@(posedge clk or negedge rst_n)
  begin
    if(~rst_n)
    begin
      upm_pu_awready <= 'd0  ;
      upm_pu_wready  <= 'd0  ;
      upm_pu_bid     <= 'd0  ;
      upm_pu_bresp   <= 2'b11;
      upm_pu_bvalid  <= 'd0;
      upm_pu_arready <= 'd0  ;
      upm_pu_rlast   <= 1'b0 ;
      upm_pu_rid     <= 'd0  ;
      upm_pu_rresp   <= 2'b11;
      upm_pu_rlast   <= 'd0  ;
      upm_pu_rvalid  <= 'd0  ;
    end
    else 
    begin
      upm_pu_awready <= 1'b1;
      upm_pu_wready  <= 1'b1;
      upm_pu_bid     <= upm_pu_awvalid ? upm_pu_awid : upm_pu_bid;
      upm_pu_bresp   <= 2'b00;
      upm_pu_bvalid  <= wr_en;
      upm_pu_arready <= 1'b1;
      upm_pu_rlast   <= 1'b1;
      upm_pu_rid     <= upm_pu_arvalid ? upm_pu_arid : upm_pu_rid;     
      upm_pu_rresp   <= 2'b00;           
      upm_pu_rlast   <= rd_en;
      upm_pu_rvalid  <= rd_en;
    end
  end
  
  assign upm_pu_rdata = {{WID_32{1'b0}},rd_data};
  
  always@(posedge clk)
  begin
    wr_en   <= upm_pu_wvalid;
    wr_addr <= upm_pu_awvalid ? upm_pu_awaddr[0 +: WID_16] : wr_addr;
    wr_data <= upm_pu_wvalid  ? upm_pu_wdata[0 +: WID_32]  : wr_data;
    rd_en   <= upm_pu_arvalid;
    rd_addr <= upm_pu_arvalid ? upm_pu_araddr[0 +: WID_16] : rd_addr;
  end

  //-- general module.
  localparam INT_VECTOR_ADDR                      = 16'h00_00;   
  localparam PA_VERSION_ADDR                      = 16'h00_08;
  localparam PA_BUILD_INFORMATION_ADDR            = 16'h00_10;
  localparam ADAPTED_MAIN_BOAD_VERSION_ADDR       = 16'h00_18;
  localparam ADAPTED_GIC_BOAD_VERSION_ADDR        = 16'h00_20;
  localparam ADAPTED_ROIC_BOAD_VERSION_ADDR       = 16'h00_28;
  localparam ADAPTED_RESERVED_BOAD_0_VERSION_ADDR = 16'h00_30;
  localparam ADAPTED_RESERVED_BOAD_1_VERSION_ADDR = 16'h00_38;
  localparam ADAPTED_RESERVED_BOAD_2_VERSION_ADDR = 16'h00_40;
  localparam PA_PU_COM_VERSION_ADDR               = 16'h00_48;
  localparam PA_RST_INIT_STATE_ADDR               = 16'h00_50;

  //-- gic module 
  localparam GIC_STR_ADDR             = 16'h02_00;
  localparam GIC_STOP_ADDR            = 16'h02_08;
  localparam GIC_REQ_CODE_ADDR        = 16'h02_10;
  localparam GIC_DOUT_EN_ADDR         = 16'h02_18;
  localparam GIC_LINE_TIME_ADDR       = 16'h02_20;
  localparam GIC_OE_RAISING_EDGE_ADDR = 16'h02_28;
  localparam GIC_OE_FALLING_EDGE_ADDR = 16'h02_30;
  localparam GIC_STR_ROW_NUM_ADDR     = 16'h02_38;
  localparam GIC_END_ROW_NUM_ADDR     = 16'h02_40;
  localparam GIC_BINNING_MODE_ADDR    = 16'h02_48;
  localparam GIC_STATE_ADDR           = 16'h03_A0;
  localparam GIC_END_ADDR             = 16'h03_A8;
  localparam GIC_DFX_ADDR             = 16'h03_C0;
  localparam GIC_DEBUG_IN_ADDR        = 16'h03_C8;
  localparam GIC_DEBUG_OUT_ADDR       = 16'h03_D0;

  //-- roic module
  localparam ROIC_STR_ADDR          = 16'h04_00;
  localparam ROIC_REQ_CODE_ADDR     = 16'h04_08;
  localparam ROIC_REG_00_ADDR       = 16'h04_10;
  localparam ROIC_REG_02_ADDR       = 16'h04_18;
  localparam ROIC_REG_05_ADDR       = 16'h04_20;
  localparam ROIC_REG_06_ADDR       = 16'h04_28;
  localparam ROIC_REG_07_ADDR       = 16'h04_30;
  localparam ROIC_REG_09_ADDR       = 16'h04_38;
  localparam ROIC_REG_0A_ADDR       = 16'h04_40;
  localparam ROIC_REG_0B_ADDR       = 16'h04_48;
  localparam ROIC_REG_0C_ADDR       = 16'h04_50;
  localparam ROIC_REG_0D_ADDR       = 16'h04_58;
  localparam ROIC_REG_0E_ADDR       = 16'h04_60;
  localparam ROIC_REG_0F_ADDR       = 16'h04_68;
  localparam ROIC_REG_10_ADDR       = 16'h04_70;
  localparam ROIC_REG_11_ADDR       = 16'h04_78;
  localparam ROIC_REG_17_ADDR       = 16'h04_80;
  localparam ROIC_REG_24_ADDR       = 16'h04_88;
  localparam ROIC_REG_28_ADDR       = 16'h04_90;
  localparam ROIC_REG_2D_ADDR       = 16'h04_98;
  localparam ROIC_REG_3B_ADDR       = 16'h04_A0;
  localparam ROIC_STR_COL_NUM_ADDR  = 16'h04_A8;
  localparam ROIC_END_COL_NUM_ADDR  = 16'h04_B0;
  localparam ROIC_BINNING_MODE_ADDR = 16'h04_B8;
  localparam ROIC_STATE_ADDR        = 16'h05_A0;
  localparam ROIC_END_ADDR          = 16'h05_A8;
  localparam ROIC_DFX_ADDR          = 16'h05_C0;
  localparam ROIC_DEBUG_IN_ADDR     = 16'h05_C8;
  localparam ROIC_DEBUG_OUT_ADDR    = 16'h05_D0;
                 
  //-- img_wr module.  
  localparam IMG_WR_STR_ADDR        = 16'h06_00;
  localparam IMG_WR_STR_ADDR_ADDR   = 16'h06_08;
  localparam IMG_WR_STATE_ADDR      = 16'h07_A0;
  localparam IMG_WR_END_ADDR        = 16'h07_A8;
  localparam IMG_WR_DFX_ADDR        = 16'h07_C0;
  localparam IMG_WR_DEBUG_IN_ADDR   = 16'h07_C8;
  localparam IMG_WR_DEBUG_OUT_ADDR  = 16'h07_D0;

  //-- img_corr module.
  localparam IMG_CORR_STR_ADDR                  = 16'h08_00;
  localparam IMG_PKG_NUM_ADDR                   = 16'h08_01;
  localparam IMG_ROW_NUM_ADDR                   = 16'h08_02;
  localparam IMG_COL_NUM_ADDR                   = 16'h08_03;
  localparam IMG_CORR_OFFSET_EN_ADDR            = 16'h08_04;
  localparam IMG_CORR_OFFSET_TEMP_STR_ADDR_ADDR = 16'h08_05;
  localparam IMG_CORR_OFFSET_ADDER_VALUE_ADDR   = 16'h08_06;
  localparam IMG_CORR_GAIN_EN_ADDR              = 16'h08_07;
  localparam IMG_CORR_GAIN_TEMP_STR_ADDR_ADDR   = 16'h08_08;
  localparam IMG_CORR_GAIN_CLIPPING_VALUE_ADDR  = 16'h08_09;
  localparam IMG_CORR_DEFECT_EN_ADDR            = 16'h08_10;
  localparam IMG_CORR_STATE_ADDR                = 16'h09_A0;
  localparam IMG_CORR_END_ADDR                  = 16'h09_A8;
  localparam IMG_CORR_DFX_ADDR                  = 16'h09_C0;
  localparam IMG_CORR_DEBUG_IN_ADDR             = 16'h09_C8;
  localparam IMG_CORR_DEBUG_OUT_ADDR            = 16'h09_D0;

  reg [WID_32-1:0] int_vector        = 'd0;
  reg [WID_32-1:0] pa_rst_init_state = 'd0;
  
  always@(posedge clk)
  begin 
    irq_pa_2_pu <= |int_vector;
  end
  
  always@(posedge clk)
  begin
    if(rd_en && (rd_addr == INT_VECTOR_ADDR))
    begin
      int_vector <= {28'd0       ,
                     img_corr_end,
                     img_wr_end  ,
                     roic_end    ,
                     gic_end     };
    end
    else
    begin
      int_vector <=  int_vector | {28'd0       ,
                                   img_corr_end,
                                   img_wr_end  ,
                                   roic_end    ,
                                   gic_end     };
    end
  end
  
  always@(posedge clk)
  begin
    pa_rst_init_state <= 32'hFF_FF_FF_FF;
  end
  
  always@(posedge clk)
  begin
    if(rd_en)
    begin
      case(rd_addr)
        //-- general module.
        INT_VECTOR_ADDR                      : rd_data <= int_vector                     ;
        PA_VERSION_ADDR                      : rd_data <= PA_VERSION                     ;
        PA_BUILD_INFORMATION_ADDR            : rd_data <= PA_BUILD_INFO                  ;
        ADAPTED_MAIN_BOAD_VERSION_ADDR       : rd_data <= ADAPTED_MAIN_BOAD_VERSION      ;
        ADAPTED_GIC_BOAD_VERSION_ADDR        : rd_data <= ADAPTED_GIC_BOAD_VERSION       ;
        ADAPTED_ROIC_BOAD_VERSION_ADDR       : rd_data <= ADAPTED_ROIC_BOAD_VERSION      ;
        ADAPTED_RESERVED_BOAD_0_VERSION_ADDR : rd_data <= ADAPTED_RESERVED_BOAD_0_VERSION;
        ADAPTED_RESERVED_BOAD_1_VERSION_ADDR : rd_data <= ADAPTED_RESERVED_BOAD_1_VERSION;
        ADAPTED_RESERVED_BOAD_2_VERSION_ADDR : rd_data <= ADAPTED_RESERVED_BOAD_2_VERSION;
        PA_PU_COM_VERSION_ADDR               : rd_data <= PA_PU_COM_VERSION              ;
        PA_RST_INIT_STATE_ADDR               : rd_data <= pa_rst_init_state              ;

        //-- gic module.      
        GIC_REQ_CODE_ADDR        : rd_data <= gic_req_code       ;      
        GIC_DOUT_EN_ADDR         : rd_data <= gic_dout_en        ;
        GIC_LINE_TIME_ADDR       : rd_data <= gic_line_time      ;
        GIC_OE_RAISING_EDGE_ADDR : rd_data <= gic_oe_raising_edge;
        GIC_OE_FALLING_EDGE_ADDR : rd_data <= gic_oe_falling_edge;
        GIC_STR_ROW_NUM_ADDR     : rd_data <= gic_str_row_num    ;
        GIC_END_ROW_NUM_ADDR     : rd_data <= gic_end_row_num    ;
        GIC_BINNING_MODE_ADDR    : rd_data <= gic_binning_mode   ;
        GIC_STATE_ADDR           : rd_data <= gic_state          ;
        GIC_END_ADDR             : rd_data <= gic_end            ;
        GIC_DFX_ADDR             : rd_data <= gic_dfx            ;
        GIC_DEBUG_IN_ADDR        : rd_data <= gic_debug_in       ;
        GIC_DEBUG_OUT_ADDR       : rd_data <= gic_debug_out      ;
      
        //-- roic module                       
        ROIC_REQ_CODE_ADDR     : rd_data <= roic_req_code    ;  
        ROIC_REG_00_ADDR       : rd_data <= roic_reg_00      ;  
        ROIC_REG_02_ADDR       : rd_data <= roic_reg_02      ;  
        ROIC_REG_05_ADDR       : rd_data <= roic_reg_05      ;  
        ROIC_REG_06_ADDR       : rd_data <= roic_reg_06      ;  
        ROIC_REG_07_ADDR       : rd_data <= roic_reg_07      ;  
        ROIC_REG_09_ADDR       : rd_data <= roic_reg_09      ;  
        ROIC_REG_0A_ADDR       : rd_data <= roic_reg_0a      ;  
        ROIC_REG_0B_ADDR       : rd_data <= roic_reg_0b      ;  
        ROIC_REG_0C_ADDR       : rd_data <= roic_reg_0c      ;  
        ROIC_REG_0D_ADDR       : rd_data <= roic_reg_0d      ;  
        ROIC_REG_0E_ADDR       : rd_data <= roic_reg_0e      ;  
        ROIC_REG_0F_ADDR       : rd_data <= roic_reg_0f      ;  
        ROIC_REG_10_ADDR       : rd_data <= roic_reg_10      ;  
        ROIC_REG_11_ADDR       : rd_data <= roic_reg_11      ;  
        ROIC_REG_17_ADDR       : rd_data <= roic_reg_17      ;  
        ROIC_REG_24_ADDR       : rd_data <= roic_reg_24      ;  
        ROIC_REG_28_ADDR       : rd_data <= roic_reg_28      ;  
        ROIC_REG_2D_ADDR       : rd_data <= roic_reg_2d      ;  
        ROIC_REG_3B_ADDR       : rd_data <= roic_reg_3b      ;  
        ROIC_STR_COL_NUM_ADDR  : rd_data <= roic_str_col_num ;  
        ROIC_END_COL_NUM_ADDR  : rd_data <= roic_end_col_num ;  
        ROIC_BINNING_MODE_ADDR : rd_data <= roic_binning_mode;  
        ROIC_STATE_ADDR        : rd_data <= roic_state       ;  
        ROIC_END_ADDR          : rd_data <= roic_end         ;  
        ROIC_DFX_ADDR          : rd_data <= roic_dfx         ;  
        ROIC_DEBUG_IN_ADDR     : rd_data <= roic_debug_in    ;  
        ROIC_DEBUG_OUT_ADDR    : rd_data <= roic_debug_out   ;  
      
        //-- img_wr module.
        IMG_WR_STR_ADDR_ADDR  : rd_data <= img_wr_str_addr ;
        IMG_WR_STATE_ADDR     : rd_data <= img_wr_state    ;
        IMG_WR_END_ADDR       : rd_data <= img_wr_end      ;
        IMG_WR_DFX_ADDR       : rd_data <= img_wr_dfx      ;
        IMG_WR_DEBUG_IN_ADDR  : rd_data <= img_wr_debug_in ;
        IMG_WR_DEBUG_OUT_ADDR : rd_data <= img_wr_debug_out;

        //-- img_corr module.
        IMG_PKG_NUM_ADDR                   : rd_data <= img_pkg_num                  ;
        IMG_ROW_NUM_ADDR                   : rd_data <= img_row_num                  ;
        IMG_COL_NUM_ADDR                   : rd_data <= img_col_num                  ;
        IMG_CORR_OFFSET_EN_ADDR            : rd_data <= img_corr_offset_en           ;
        IMG_CORR_OFFSET_TEMP_STR_ADDR_ADDR : rd_data <= img_corr_offset_temp_str_addr;
        IMG_CORR_OFFSET_ADDER_VALUE_ADDR   : rd_data <= img_corr_offset_adder_value  ;
        IMG_CORR_GAIN_EN_ADDR              : rd_data <= img_corr_gain_en             ;
        IMG_CORR_GAIN_TEMP_STR_ADDR_ADDR   : rd_data <= img_corr_gain_temp_str_addr  ;
        IMG_CORR_GAIN_CLIPPING_VALUE_ADDR  : rd_data <= img_corr_gain_clipping_value ;
        IMG_CORR_DEFECT_EN_ADDR            : rd_data <= img_corr_defect_en           ;
        IMG_CORR_STATE_ADDR                : rd_data <= img_corr_state               ;
        IMG_CORR_DFX_ADDR                  : rd_data <= img_corr_dfx                 ;
        IMG_CORR_DEBUG_IN_ADDR             : rd_data <= img_corr_debug_in            ;
        IMG_CORR_DEBUG_OUT_ADDR            : rd_data <= img_corr_debug_out           ;
        
        default : rd_data <= 32'hdeadbeef;
      endcase
    end
    else
    begin
      rd_data <= rd_data;
    end
  end
  
  //-- gic module.
  always@(posedge clk or negedge rst_n)
  begin
    if(~rst_n)
    begin
      gic_str  <= 'd0;
      gic_stop <= 'd0;
    end
    else 
    begin
      gic_str  <= (wr_en && (wr_addr == GIC_STR_ADDR )) ? wr_data[0] : 1'b0;
      gic_stop <= (wr_en && (wr_addr == GIC_STOP_ADDR)) ? wr_data[0] : 1'b0;
    end
  end 
  
  always@(posedge clk)
  begin
    gic_req_code        <= (wr_en && (wr_addr == GIC_REQ_CODE_ADDR       )) ? wr_data[WID_8-1:0]  : gic_req_code       ;
    gic_dout_en         <= (wr_en && (wr_addr == GIC_DOUT_EN_ADDR        )) ? wr_data[0]          : gic_dout_en        ;
    gic_line_time       <= (wr_en && (wr_addr == GIC_LINE_TIME_ADDR      )) ? wr_data[WID_32-1:0] : gic_line_time      ;
    gic_oe_raising_edge <= (wr_en && (wr_addr == GIC_OE_RAISING_EDGE_ADDR)) ? wr_data[WID_32-1:0] : gic_oe_raising_edge;
    gic_oe_falling_edge <= (wr_en && (wr_addr == GIC_OE_FALLING_EDGE_ADDR)) ? wr_data[WID_32-1:0] : gic_oe_falling_edge;
    gic_str_row_num     <= (wr_en && (wr_addr == GIC_STR_ROW_NUM_ADDR    )) ? wr_data[WID_16-1:0] : gic_str_row_num    ;
    gic_end_row_num     <= (wr_en && (wr_addr == GIC_END_ROW_NUM_ADDR    )) ? wr_data[WID_16-1:0] : gic_end_row_num    ;
    gic_binning_mode    <= (wr_en && (wr_addr == GIC_BINNING_MODE_ADDR   )) ? wr_data[WID_8-1:0]  : gic_binning_mode   ;
    gic_debug_in        <= (wr_en && (wr_addr == GIC_DEBUG_IN_ADDR       )) ? wr_data[WID_32-1:0] : gic_debug_in       ;
  end               
  
  //-- roic module  
  always@(posedge clk or negedge rst_n)
  begin
    if(~rst_n)
    begin
      roic_str  <= 'd0;
    end
    else 
    begin
      roic_str  <= (wr_en && (wr_addr == ROIC_STR_ADDR )) ? wr_data[0] : 1'b0;
    end
  end   
  
  always@(posedge clk)
  begin
    roic_req_code     <= (wr_en && (wr_addr == ROIC_REQ_CODE_ADDR    )) ? wr_data[WID_8-1:0]  : roic_req_code    ;
    roic_reg_00       <= (wr_en && (wr_addr == ROIC_REG_00_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_00      ;
    roic_reg_02       <= (wr_en && (wr_addr == ROIC_REG_02_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_02      ;
    roic_reg_05       <= (wr_en && (wr_addr == ROIC_REG_05_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_05      ;
    roic_reg_06       <= (wr_en && (wr_addr == ROIC_REG_06_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_06      ;
    roic_reg_07       <= (wr_en && (wr_addr == ROIC_REG_07_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_07      ;
    roic_reg_09       <= (wr_en && (wr_addr == ROIC_REG_09_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_09      ;
    roic_reg_0a       <= (wr_en && (wr_addr == ROIC_REG_0A_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_0a      ;
    roic_reg_0b       <= (wr_en && (wr_addr == ROIC_REG_0B_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_0b      ;
    roic_reg_0c       <= (wr_en && (wr_addr == ROIC_REG_0C_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_0c      ;
    roic_reg_0d       <= (wr_en && (wr_addr == ROIC_REG_0D_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_0d      ;
    roic_reg_0e       <= (wr_en && (wr_addr == ROIC_REG_0E_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_0e      ;
    roic_reg_0f       <= (wr_en && (wr_addr == ROIC_REG_0F_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_0f      ;
    roic_reg_10       <= (wr_en && (wr_addr == ROIC_REG_10_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_10      ;
    roic_reg_11       <= (wr_en && (wr_addr == ROIC_REG_11_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_11      ;
    roic_reg_17       <= (wr_en && (wr_addr == ROIC_REG_17_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_17      ;
    roic_reg_24       <= (wr_en && (wr_addr == ROIC_REG_24_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_24      ;
    roic_reg_28       <= (wr_en && (wr_addr == ROIC_REG_28_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_28      ;
    roic_reg_2d       <= (wr_en && (wr_addr == ROIC_REG_2D_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_2d      ;
    roic_reg_3b       <= (wr_en && (wr_addr == ROIC_REG_3B_ADDR      )) ? wr_data[WID_16-1:0] : roic_reg_3b      ;
    roic_str_col_num  <= (wr_en && (wr_addr == ROIC_STR_COL_NUM_ADDR )) ? wr_data[WID_16-1:0] : roic_str_col_num ;
    roic_end_col_num  <= (wr_en && (wr_addr == ROIC_END_COL_NUM_ADDR )) ? wr_data[WID_16-1:0] : roic_end_col_num ;
    roic_binning_mode <= (wr_en && (wr_addr == ROIC_BINNING_MODE_ADDR)) ? wr_data[WID_8-1:0]  : roic_binning_mode;
    roic_debug_in     <= (wr_en && (wr_addr == ROIC_DEBUG_IN_ADDR    )) ? wr_data[WID_32-1:0] : roic_debug_in    ;
  end 
  
  //-- img_wr module.
  always@(posedge clk or negedge rst_n)
  begin
    if(~rst_n)
    begin
      img_wr_str <= 'd0;
    end
    else 
    begin
      img_wr_str <= (wr_en && (wr_addr == IMG_WR_STR_ADDR )) ? wr_data[0] : 1'b0;
    end
  end   
  
  always@(posedge clk)
  begin
    img_wr_str_addr <= (wr_en && (wr_addr == IMG_WR_STR_ADDR_ADDR )) ? wr_data[WID_32-1:0] : img_wr_str_addr ;
    img_wr_debug_in <= (wr_en && (wr_addr == IMG_WR_DEBUG_IN_ADDR )) ? wr_data[WID_32-1:0] : img_wr_debug_in ;
  end 
  
  //-- img_corr module.
  always@(posedge clk or negedge rst_n)
  begin
    if(~rst_n)
    begin
      img_corr_str <= 1'b0;
    end
    else 
    begin
      img_corr_str <= (wr_en && (wr_addr == IMG_CORR_STR_ADDR)) ? wr_data[0] : 1'b0;
    end
  end 
  
  always@(posedge clk)
  begin
    img_pkg_num                   <= (wr_en && (wr_addr == IMG_PKG_NUM_ADDR                  )) ? wr_data[WID_16-1:0] : img_pkg_num                  ;
    img_row_num                   <= (wr_en && (wr_addr == IMG_ROW_NUM_ADDR                  )) ? wr_data[WID_16-1:0] : img_row_num                  ;
    img_col_num                   <= (wr_en && (wr_addr == IMG_COL_NUM_ADDR                  )) ? wr_data[WID_16-1:0] : img_col_num                  ;
    img_corr_offset_en            <= (wr_en && (wr_addr == IMG_CORR_OFFSET_EN_ADDR           )) ? wr_data[0]          : img_corr_offset_en           ;
    img_corr_offset_temp_str_addr <= (wr_en && (wr_addr == IMG_CORR_OFFSET_TEMP_STR_ADDR_ADDR)) ? wr_data[WID_32-1:0] : img_corr_offset_temp_str_addr;
    img_corr_offset_adder_value   <= (wr_en && (wr_addr == IMG_CORR_OFFSET_ADDER_VALUE_ADDR  )) ? wr_data[WID_16-1:0] : img_corr_offset_adder_value  ;
    img_corr_gain_en              <= (wr_en && (wr_addr == IMG_CORR_GAIN_EN_ADDR             )) ? wr_data[0]          : img_corr_gain_en             ;
    img_corr_gain_temp_str_addr   <= (wr_en && (wr_addr == IMG_CORR_GAIN_TEMP_STR_ADDR_ADDR  )) ? wr_data[WID_32-1:0] : img_corr_gain_temp_str_addr  ;
    img_corr_gain_clipping_value  <= (wr_en && (wr_addr == IMG_CORR_GAIN_CLIPPING_VALUE_ADDR )) ? wr_data[WID_16-1:0] : img_corr_gain_clipping_value ;
    img_corr_defect_en            <= (wr_en && (wr_addr == IMG_CORR_DEBUG_IN_ADDR            )) ? wr_data[0]          : img_corr_defect_en           ;
    img_corr_debug_in             <= (wr_en && (wr_addr == IMG_CORR_DEFECT_EN_ADDR           )) ? wr_data[WID_32-1:0] : img_corr_debug_in            ;
  end
  
generate

  if(DBG_EN)
  begin
  
  
  end

endgenerate
  
endmodule
